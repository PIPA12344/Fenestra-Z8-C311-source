"""Five independently owned WLM native recipes from pinned C311 sources.

No BOOT file, precomposed feature MAIN, camera or QEMU is used.  Shared
resources are explicit owners and conditional variants, never last-writer wins.
"""
from dataclasses import dataclass, replace
import binascii
import hashlib
import json
import struct
from z8_native_profile import BASE, check_main
from z8_feature_suite import checked_upstream, VENDOR, COMPILED_SHA256
from z8_native_capsule import _runtime_address, COPY_END_SITE

COMPONENTS=('framing','prores','h265','log_colour','photo_tools')
BITS={name:1<<i for i,name in enumerate(COMPONENTS)}
SHARED_LOCALE='shared.locale'
SHARED_VIDEO='shared.menu'
SHARED_QUALITY='shared.quality'
QUALITY_CODE=0x40a41000
QUALITY_REPUBLISH=0x40a41250
VIDEO_CODE=0x40980000
VIDEO_CONFIG=0x40980f00
VIDEO_ROWS=0x40a30000
VIDEO_EVENTS=0x40a31000
V051_VIDEO_CONFIG=0x40981000
V051_ROUTE_CODE=0x40980100
V051_LOCALE=dict(languages=36,text_count=3251,offsets_directory=0x4327c5a0,pools_directory=0x439daf58)
CONDITION_SITE=0x403a036c
ICON_SITES=(0x4032dc64,0x4032dd34)
INIT_OFFSET=0x4b13000
INIT_RAM=0xa02808000
MOV_CLOSE=0x40ceef10


def sha(data):return hashlib.sha256(data).hexdigest()
def selected(mask):
    if type(mask)is not int or not 1<=mask<=31:raise ValueError('nonempty exact five-component mask required')
    return [name for name in COMPONENTS if mask&BITS[name]]

@dataclass(frozen=True)
class OwnedWindow:
    offset:int
    old:bytes
    new:bytes
    owner:str
    purpose:str
    @property
    def address(self):return _runtime_address(self.offset,len(self.new))[0]
    def metadata(self):
        return dict(offset=hex(self.offset),address=hex(self.address),bytes=len(self.new),owner=self.owner,purpose=self.purpose,old_sha256=sha(self.old),new_sha256=sha(self.new))

@dataclass(frozen=True)
class NativeRecipe:
    mask:int
    image:bytes
    windows:tuple
    detail:dict
    def metadata(self):
        return dict(schema='fenestra.wlm-native-five-owner-recipe/v1',mask=self.mask,components=selected(self.mask),image_sha256=sha(self.image),windows=[w.metadata()for w in self.windows],detail=self.detail)


def _unit(name):
    from z8patch.compiled import load
    raw,metadata=load(name,'prebuilt','clang')
    return raw,{k:int(v['va'],16)for k,v in metadata['symbols'].items()}


def shared_video_code():
    raw,entries=_unit('shared');code=bytearray(raw)
    # Exact pinned instructions; preserve all branch destinations and registers.
    for offset,old,new in ((0x5c,'0c011036','0c011836'),(0x70,'6c000836','6c001036')):
        if bytes(code[offset:offset+4])!=bytes.fromhex(old):raise ValueError('shared bank instruction preimage differs')
        code[offset:offset+4]=bytes.fromhex(new)
    return bytes(code),entries


def _shared_video_code_v051():
    raw,entries=_unit('shared')
    code=bytearray(raw)
    # Upstream has one video-quality bit. Independent H.265 and Log use
    # different bits; ProRes and Framing retain the upstream bit positions.
    for offset,old,new in ((0x64,'0c011036','0c011836'),(0x78,'6c000836','6c001036')):
        if code[offset:offset+4]!=bytes.fromhex(old):
            raise ValueError('v0.5.1 shared bank instruction preimage differs')
        code[offset:offset+4]=bytes.fromhex(new)
    old_literals=struct.pack('<2Q',VIDEO_CONFIG,VIDEO_CONFIG+8)
    if code[-16:]!=old_literals:
        raise ValueError('v0.5.1 shared bank configuration literals moved')
    code[-16:]=struct.pack('<2Q',V051_VIDEO_CONFIG,V051_VIDEO_CONFIG+8)
    return bytes(code),entries


def quality_part(eg,prores,h265,prores_ok,h265_ok):
    from z8quality.patcher import build_main,tables
    from z8patch.image import branch
    code,entries=_unit('quality')
    part,detail=build_main(eg,code,entries,h265_ok if h265 else 0,prores_ok if prores else 0)
    raw=bytearray(next(e['new']for e in part.edits if e['offset']==QUALITY_CODE-BASE))
    patches=[]
    # The shared icon router must be stock-equivalent for an absent codec,
    # even if unrelated/persisted spare DS bytes happen to be nonzero.
    gates=[]
    if not h265:gates.extend((at,'29090051',end)for at,end in ((0x1c,0x80),(0xa4,0x108),(0x164,0x1c8)))
    if not prores:gates.extend((at,old,end)for at,old,end in ((0x48,'491b0058',0x80),(0xd0,'09170058',0x108),(0x190,'09110058',0x1c8)))
    for at,old,end in gates:
        if raw[at:at+4]!=bytes.fromhex(old):raise ValueError('quality router gate preimage differs')
        replacement=branch(QUALITY_CODE+at,QUALITY_CODE+end);raw[at:at+4]=replacement
        patches.append(dict(address=hex(QUALITY_CODE+at),old=old,new=replacement.hex()))
    pairs,new,hq,pngs=tables(eg)
    for name,data,enabled in (('tier_star_pairs',pairs,h265),('tier_star_new',new,h265),('tier_hq_pairs',hq,prores)):
        if not enabled:
            at=entries[name]-QUALITY_CODE;raw[at:at+len(data)]=bytes(len(data))
    kept=[]
    for e in part.edits:
        address=BASE+e['offset']
        if address==0x40a20000 and not h265:continue
        if address in (0x440c7b58,0x440c7b60)and not h265:continue
        if address==0x440c7b68 and not prores:continue
        e=dict(e)
        if address==QUALITY_CODE:e['new']=bytes(raw)
        kept.append(e)
    detail.update(prores_enabled=prores,h265_enabled=h265,router_gates=patches,republish=hex(QUALITY_REPUBLISH),selected_icon_data_only=True)
    return kept,detail


def _emit_video(img,menu,mask,callbacks,put,*,code_builder=shared_video_code,config=VIDEO_CONFIG):
    from z8patch.image import branch
    if not mask&(BITS['framing']|BITS['prores']|BITS['h265']|BITS['log_colour']):return None
    records={item:(row,pointer)for item,row,pointer in zip(menu.items,menu.rows,menu.pointers)}
    records.update(menu.replacements);order=list(menu.items)
    if mask&BITS['log_colour']:
        order.remove(0x2e38);order.insert(order.index(0x2fc1)+1,0x2e38)
    if mask&(BITS['prores']|BITS['h265']):
        if mask&BITS['prores']:order.remove(0x311b)
        insert=[]
        if mask&BITS['prores']:insert.append(0x311b)
        if mask&BITS['h265']:insert.append(0x315a)
        at=order.index(0x3118)+1;order[at:at]=insert
    if set(order)!=set(records)or len(order)!=len(set(order)):raise ValueError('video menu contribution loses/duplicates a row')
    rows=b''.join(records[i][0]for i in order);pointers=struct.pack('<'+'Q'*len(order),*(records[i][1]for i in order))
    if VIDEO_ROWS+len(rows)>VIDEO_EVENTS or VIDEO_EVENTS+len(pointers)>0x40a40000:raise ValueError('video menu capacity')
    put(VIDEO_ROWS-BASE,rows,'shared final video menu rows',SHARED_VIDEO,expect_zero=True)
    put(VIDEO_EVENTS-BASE,pointers,'shared final video menu transition pointers',SHARED_VIDEO,expect_zero=True)
    put(0x4bcc2b0+0x10,struct.pack('<QQH',VIDEO_ROWS,VIDEO_EVENTS,len(order)),'shared video menu page: final rows and count',SHARED_VIDEO,expect=struct.pack('<QQH',BASE+0x3aa4758,BASE+0x3aa4ee8,44))
    for enabled,site,item in ((mask&BITS['log_colour'],0x40572940,0x2e38),(mask&BITS['prores'],0x4057295c,0x311b),(mask&BITS['framing'],0x40572978,0x310f)):
        if enabled:put(site-BASE,struct.pack('<I',0x52800002),'show selected menu item '+hex(item),SHARED_VIDEO,expect=bytes.fromhex('e2030032'))
    code,entries=code_builder()
    put(VIDEO_CODE-BASE,code,'shared five-component bank and refresh code',SHARED_VIDEO,expect_zero=True)
    callback_values=[callbacks.get(k,0)for k in ('prores','h265','anamorphic','log')]
    put(config-BASE,struct.pack('<II4Q',mask&15,0,*callback_values),'shared five-component mask and row callbacks',SHARED_VIDEO,expect_zero=True)
    put(0x4bf3a80,struct.pack('<Q',entries['shared_refresh']),'shared video refresh dispatcher',SHARED_VIDEO,expect=struct.pack('<Q',0x40572b2c))
    put(0x410162ec-BASE,branch(0x410162ec,entries['shared_bank']),'shared menu-bank copy dispatcher',SHARED_VIDEO,expect=bytes.fromhex('ea2340a9'))
    return dict(items=[hex(i)for i in order],count=len(order),mask=mask&15,callbacks={k:hex(v)for k,v in callbacks.items()},entries={k:hex(v)for k,v in entries.items()})


def _projector(stock):
    from z8prores.menu import Image
    from z8patch.shared import VideoMenu

    img=Image(stock+binascii.crc_hqx(stock,0).to_bytes(2,'big'))
    menu=VideoMenu(img.old)
    owners={}
    def put(offset,data,purpose,owner,**options):
        img.put(offset,data,purpose,**options);owners[offset]=owner
    def merge(owner,edits,refinements=()):
        for e in edits:
            if menu.consumed(e):continue
            overlaps=[x for x in img.edits if e['offset']<x['end']and x['offset']<e['end']]
            if overlaps:
                if BASE+e['offset']not in refinements or len(overlaps)!=1:raise ValueError('unregistered native component overlap')
                previous=overlaps[0]
                if not previous['offset']<=e['offset']<=e['end']<=previous['end']or bytes(img.new[e['offset']:e['end']])!=e['old']:raise ValueError('native refinement base differs')
                at=e['offset']-previous['offset'];data=bytearray(previous['new']);data[at:at+len(e['new'])]=e['new'];previous['new']=bytes(data);previous['purpose']+='; '+e['purpose'];img.new[e['offset']:e['end']]=e['new']
            else:put(e['offset'],e['new'],e['purpose'],owner,expect=e['old'])
    return img,menu,owners,put,merge


def _projection(stock,mask,img,owners,detail,compiled_sha):
    windows=[];end=0;rebuilt=bytearray(stock)
    for e in sorted(img.edits,key=lambda e:e['offset']):
        at=e['offset'];data=bytes(e['new']);old=bytes(e['old'])
        if at<end or stock[at:at+len(data)]!=old:raise ValueError('overlap/stock preimage differs')
        owner=owners.get(at)
        if owner is None:raise ValueError('native changed window has no owner')
        windows.append(OwnedWindow(at,old,data,owner,e['purpose']));rebuilt[at:at+len(data)]=data;end=at+len(data)
    image=bytes(img.new[:-2])
    if bytes(rebuilt)!=image:raise ValueError('recipe does not cover every changed MAIN byte')
    detail.update(compiled_metadata_sha256=compiled_sha,codec_independence=True,shared_bank_bits=BITS,unselected_backend_images_absent=True)
    return NativeRecipe(mask,image,tuple(windows),detail)


def compose_recipe(stock,mask):
    """Project the historical v0.4.0-rc.2 WLM suite."""
    selected(mask);check_main(stock);checked_upstream()
    from z8patch.shared import Strings,loader

    img,menu,owners,put,merge=_projector(stock)
    strings=Strings();detail={};callbacks={}
    if mask&BITS['framing']:
        from z8opengate.patcher import build_main
        code,entries=_unit('opengate');part,detail['opengate']=build_main(img.old,code,entries,strings);merge('framing',part.edits)
        from z8anamorphic.patcher import build_main
        code,entries=_unit('anamorphic');part,detail['anamorphic']=build_main(bytes(part.new),code,entries,strings);menu.collect(part,0x310f);merge('framing',part.edits,(0x40960c98,0x40962b58,0x409616a4,0x407138d0));callbacks['anamorphic']=entries['ap_menu_refresh_hook']
    star,star_entries=_unit('quality');prores_ok=h265_ok=0
    if mask&BITS['prores']:
        from z8prores.menu import build_main
        part,detail['prores']=build_main(img.old,strings,star_entries['tier_hq_ok']);menu.collect(part,0x311b);merge('prores',part.edits);callbacks['prores']=int(detail['prores']['entries']['refresh'],16);prores_ok=int(detail['prores']['entries']['ok'],16)
    if mask&BITS['h265']:
        from z8h265.patcher import build_main
        code,entries=_unit('h265');part,detail['h265']=build_main(img.old,code,entries,strings,star_entries['tier_ok_star']);m=detail['h265']['menu'];menu.add(0x315a,bytes.fromhex(m['row_hex']),int(m['row_table'],16));merge('h265',part.edits);callbacks['h265']=entries['tier_refresh_hook'];h265_ok=entries['tier_ok_hook']
    if mask&(BITS['prores']|BITS['h265']):
        edits,detail['quality']=quality_part(img.old,bool(mask&BITS['prores']),bool(mask&BITS['h265']),prores_ok,h265_ok)
        for e in edits:
            address=BASE+e['offset'];owner='h265'if address in (0x440c7b58,0x440c7b60,0x40a20000)else'prores'if address==0x440c7b68 else SHARED_QUALITY
            merge(owner,[e])
    if mask&BITS['log_colour']:
        from z8log.patcher import build_main
        code,entries=_unit('log');part,detail['log_colour']=build_main(img.old,code,entries,strings);menu.collect(part,0x2e38);merge('log_colour',part.edits);callbacks['log']=entries['lg_menu_refresh_hook']
    if mask&BITS['photo_tools']:
        from z8precapture.patcher import edits
        from z8patch.compiled import load
        code,metadata=load('precapture','prebuilt','clang')
        for e in edits(code,metadata['symbols']):put(e['offset'],e['new'],e['purpose'],'photo_tools',expect=img.old[e['offset']:e['offset']+len(e['new'])])
        from z8pixelshift.patcher import build_main
        code,metadata=load('pixelshift','prebuilt','clang');af,_=load('pixelshift-af','prebuilt','clang');part,detail['pixelshift']=build_main(img.old,code,metadata['symbols'],af,strings);merge('photo_tools',part.edits)
    first=len(img.edits);detail['strings']=strings.emit(img)
    for e in img.edits[first:]:owners[e['offset']]=SHARED_LOCALE
    detail['menu']=_emit_video(img,menu,mask,callbacks,put)
    legacy=[]
    if mask&BITS['framing']:legacy.append('framing')
    if mask&(BITS['prores']|BITS['h265']):legacy.append('video-quality')
    if mask&BITS['log_colour']:legacy.append('log-colour')
    if mask&BITS['photo_tools']:legacy.append('photo-tools')
    first=len(img.edits);detail['loader']=loader(img,legacy)
    for e in img.edits[first:]:owners[e['offset']]='foundation.reserve-only'
    return _projection(stock,mask,img,owners,detail,COMPILED_SHA256)


def _emit_v051_text(img,strings,put):
    from z8patch.shared import STRING_IDS
    from z8_wlm_v051_locale import FIRST_EXTENDED, GROUP_SELECTORS

    if STRING_IDS != {name: (first, size) for name, (first, size, _) in GROUP_SELECTORS.items()}:
        raise ValueError('v0.5.1 text ownership differs from pinned upstream IDs')
    rows=[]
    for language in range(V051_LOCALE['languages']):
        translations={}
        for name,group in strings.groups.items():
            first,size=STRING_IDS[name]
            translations.update(zip(range(first,first+size),group[language]))
        rows.append(tuple(translations.get(ident,'').encode('utf-16le')
                          for ident in range(FIRST_EXTENDED,V051_LOCALE['text_count'])))
    return dict(features=sorted(strings.groups),**_emit_v051_text_rows(img.old,rows,put))


def _emit_v051_text_rows(stock,rows,put,capacities=None,aliases=None):
    from z8patch.shared import TABLES,TABLES_END,TEXT
    from z8_wlm_v051_locale import FIRST_EXTENDED

    layout=V051_LOCALE
    offsets=layout['offsets_directory']-BASE
    pools=layout['pools_directory']-BASE
    count=layout['text_count']
    text,tables=bytearray(),bytearray()
    directories=[]
    for language,row in enumerate(rows):
        owner=language if aliases is None else aliases[language]
        if owner!=language:
            directory_entry=directories[owner]
            pool_pointer=stock[pools+8*owner:pools+8*(owner+1)]
            put(pools+8*language,pool_pointer,f'shared language pool alias {language}',SHARED_LOCALE)
            put(offsets+8*language,struct.pack('<Q',directory_entry),f'shared language directory {language}',SHARED_LOCALE)
            directories.append(directory_entry)
            continue
        table=struct.unpack_from('<Q',stock,offsets+8*language)[0]-BASE
        pool=struct.unpack_from('<Q',stock,pools+8*language)[0]-BASE
        language_offsets=list(struct.unpack_from(f'<{FIRST_EXTENDED}i',stock,table))
        bank_start=len(text)
        for translation in row:
            language_offsets.append(TEXT+len(text)-BASE-pool)
            text.extend(translation)
        language_offsets.append(TEXT+len(text)-BASE-pool)
        if capacities is not None:
            # The getter uses adjacent offsets for the exact byte count. Pad
            # outside the final ID so the next language starts at a fixed VA.
            text.extend(bytes(capacities[language]-(len(text)-bank_start)))
        directory_entry=TABLES+len(tables)
        tables.extend(struct.pack(f'<{count+1}i',*language_offsets))
        put(offsets+8*language,struct.pack('<Q',directory_entry),f'shared language directory {language}',SHARED_LOCALE)
        directories.append(directory_entry)
    if TEXT+len(text)>TABLES or TABLES+len(tables)>TABLES_END:
        raise ValueError('v0.5.1 shared strings overflow')
    put(TEXT-BASE,text,'shared selected-feature strings',SHARED_LOCALE,expect_zero=True)
    put(TABLES-BASE,tables,'shared 36 language offset tables',SHARED_LOCALE,expect_zero=True)
    return dict(entries=count+1,text_count=count,
        text_bytes=len(text),table_bytes=len(tables),text=hex(TEXT),tables=hex(TABLES),
        **(dict(language_table_aliases={language: owner for language,owner in enumerate(aliases) if owner!=language})
           if aliases is not None else {}))


def compact_v051_locales(stock,without_prores,with_prores):
    """Build the two deterministic text banks from pinned upstream translations."""
    from z8_wlm_v051_locale import FIRST_EXTENDED, GROUP_SELECTORS, OPEN_GATE_FILE_TYPE_HELP, resolve_texts

    complete=resolve_texts(with_prores.image)
    original=resolve_texts(without_prores.image)
    before=[]
    after=[]
    aliases=[]
    banks={}
    directory=V051_LOCALE['offsets_directory']-BASE
    for language,(reference,row) in enumerate(zip(original,complete)):
        ident=OPEN_GATE_FILE_TYPE_HELP
        before.append((*row[FIRST_EXTENDED:ident],reference[ident],*row[ident+1:]))
        after.append(row[FIRST_EXTENDED:])
        table=struct.unpack_from('<Q',stock,directory+8*language)[0]-BASE
        # A table can share its pool only when stock offsets and exact text in
        # both ProRes states agree, including all stock IDs and the full bank.
        key=(stock[table:table+4*FIRST_EXTENDED],row,reference[:FIRST_EXTENDED],before[-1])
        aliases.append(banks.setdefault(key,language))
    capacities=[max(sum(map(len,a)),sum(map(len,b))) for a,b in zip(before,after)]

    variants={}
    for value,rows in ((0,before),(BITS['prores'],after)):
        windows=[]

        def put(offset,data,purpose,owner,*,expect_zero=False):
            old=stock[offset:offset+len(data)]
            if expect_zero and any(old):
                raise ValueError('compact locale overlaps nonempty stock data')
            windows.append(OwnedWindow(offset,old,bytes(data),owner,purpose))

        layout=_emit_v051_text_rows(stock,rows,put,capacities,aliases)
        variants[value]=(tuple(windows),dict(pooled_features=sorted(GROUP_SELECTORS),**layout))
    return variants


def apply_v051_locale(recipe,locale):
    """Swap the sparse reference windows for the selection's compact bank."""
    locale_windows,layout=locale
    image=bytearray(recipe.image)
    other_windows=[]
    for window in recipe.windows:
        if window.owner==SHARED_LOCALE:
            image[window.offset:window.offset+len(window.old)]=window.old
        else:
            other_windows.append(window)
    for window in locale_windows:
        if image[window.offset:window.offset+len(window.old)]!=window.old:
            raise ValueError('compact locale window overlaps selected recipe data')
        image[window.offset:window.offset+len(window.new)]=window.new
    windows=tuple(sorted((*other_windows,*locale_windows),key=lambda window:window.offset))
    return replace(recipe,image=bytes(image),windows=windows,
                   detail=dict(recipe.detail,strings=dict(recipe.detail['strings'],**layout)))


def _emit_v051_routes(routes,put):
    from z8patch.image import branch
    from z8patch.photo_shared import branch_target
    from z8opengate.e1 import OG_NEVER
    from z8area.icons_wide import NEW_BASE,NEW_COUNT

    result={}
    for site,providers in routes.items():
        preimages={row['old'] for row in providers.values()}
        if len(preimages)!=1:
            raise ValueError('shared route providers disagree on stock instruction')
        targets={name:branch_target(row) for name,row in providers.items()}
        if len(targets)==1:
            target=next(iter(targets.values()))
        elif site==CONDITION_SITE and targets.keys()=={'framing','photo_tools'}:
            target=V051_ROUTE_CODE
            code=struct.pack('<4I',0x52800010|((OG_NEVER&0xffff)<<5),
                0x72a00010|((OG_NEVER>>16)<<5),0x6b10001f,0x54000041)
            code+=branch(target+16,targets['framing'])+branch(target+20,targets['photo_tools'])
            put(target-BASE,code,'Open Gate sentinel, SDR HEIF and stock conditions',SHARED_VIDEO,expect_zero=True)
        elif site in ICON_SITES and targets.keys()=={'quality','photo_tools'}:
            by_pointer=site==ICON_SITES[0]
            target=V051_ROUTE_CODE+(0x40 if by_pointer else 0x80)
            # w9 is the scratch register used by both upstream accessors.
            # Preserve the input number and output pointers for the selected tail.
            code=struct.pack('<5I',0xb9400009 if by_pointer else 0x2a0003e9,
                0x51400129|((NEW_BASE>>12)<<10),0x51000129|((NEW_BASE&0xfff)<<10),
                0x7100013f|(NEW_COUNT<<10),0x54000042)
            code+=branch(target+20,targets['photo_tools'])+branch(target+24,targets['quality'])
            put(target-BASE,code,'still crop icon range before movie quality and stock accessors',SHARED_VIDEO,expect_zero=True)
        else:
            raise ValueError(f'unexpected providers for shared route {site:#x}')
        put(site-BASE,branch(site,target),f'shared native route {site:#x}',SHARED_VIDEO,expect=preimages.pop())
        result[hex(site)]=dict(target=hex(target),providers={name:hex(va) for name,va in targets.items()})
    return result


def _compose_recipe_v051(stock,mask,photo=None):
    """Worker entry using the isolated, verified upstream v0.5.1 snapshot."""
    from z8_feature_suite_v051 import COMPILED_SHA256 as compiled_sha
    from z8patch.shared import Strings

    selected(mask);check_main(stock)
    img,menu,owners,put,merge=_projector(stock)
    strings=Strings();detail={};callbacks={};routes={}
    framing,prores,h265,log,still=(bool(mask&BITS[name]) for name in COMPONENTS)

    def merge_with_routes(owner,edits,refinements=(),route_sites=(),route_name=None):
        retained=[]
        for row in edits:
            site=BASE+row['offset']
            if site in route_sites:
                providers=routes.setdefault(site,{})
                key=route_name or owner
                if key in providers:
                    raise ValueError(f'duplicate {key} route at {site:#x}')
                providers[key]=row
            else:
                retained.append(row)
        merge(owner,retained,refinements)

    if framing:
        from z8opengate.patcher import build_main as opengate
        from z8opengate.fix_sites import build_main as fixes,REFINED
        from z8anamorphic.patcher import build_main as anamorphic
        code,og_entries=_unit('opengate')
        part,detail['opengate']=opengate(img.old,code,og_entries,strings)
        menu.collect(part,0x2fc1)
        merge_with_routes('framing',part.edits,route_sites=(CONDITION_SITE,))
        code,entries=_unit('anamorphic')
        part,detail['anamorphic']=anamorphic(bytes(part.new),code,entries,strings)
        menu.collect(part,0x310f)
        merge('framing',part.edits,(0x40960c98,0x40962b58,0x409616a4,0x407138d0))
        callbacks['anamorphic']=entries['ap_menu_refresh_hook']
        code,fix_entries=_unit('framing-fixes')
        part,detail['framing-fixes']=fixes(bytes(img.new),code,fix_entries,og_entries,entries)
        merge('framing',part.edits,REFINED)
        del part

    prores_ok=h265_ok=0
    if prores or h265:
        _,star_entries=_unit('quality')
    if prores:
        from z8prores.menu import build_main
        from z8prores.help_text import family_help
        if framing:
            for translations in strings.groups['opengate-e1']:
                translations[-1]=family_help(translations[-1])
        part,detail['prores']=build_main(img.old,strings,star_entries['tier_hq_ok'])
        menu.collect(part,0x311b)
        merge('prores',part.edits)
        callbacks['prores']=int(detail['prores']['entries']['refresh'],16)
        prores_ok=int(detail['prores']['entries']['ok'],16)
        del part
    if h265:
        from z8h265.patcher import build_main
        code,entries=_unit('h265')
        part,detail['h265']=build_main(img.old,code,entries,strings,star_entries['tier_ok_star'])
        row=detail['h265']['menu']
        menu.add(0x315a,bytes.fromhex(row['row_hex']),int(row['row_table'],16))
        merge('h265',part.edits)
        callbacks['h265']=entries['tier_refresh_hook']
        h265_ok=entries['tier_ok_hook']
        del part
    if prores or h265:
        edits,detail['quality']=quality_part(img.old,prores,h265,prores_ok,h265_ok)
        for row in edits:
            site=BASE+row['offset']
            if site in (0x440c7b58,0x440c7b60,0x40a20000):
                owner='h265'
            elif site==0x440c7b68:
                owner='prores'
            else:
                owner=SHARED_QUALITY
            merge_with_routes(owner,[row],route_sites=ICON_SITES,route_name='quality')
    if log:
        from z8log.patcher import build_main
        code,entries=_unit('log')
        part,detail['log_colour']=build_main(img.old,code,entries,strings)
        menu.collect(part,0x2e38)
        merge('log_colour',part.edits)
        callbacks['log']=entries['lg_menu_refresh_hook']
        del part
    if still:
        from z8_wlm_photo_v051 import emit_photo
        args=() if photo is None else (photo,)
        edits,detail['photo_tools']=emit_photo(img.old,strings,_unit,*args)
        merge_with_routes('photo_tools',edits,route_sites=(CONDITION_SITE,*ICON_SITES))
    if framing:
        from z8opengate.e1 import FT_ROWS,refresh_file_type_rows
        detail['opengate']['e1']['file_type_page']['rows_rebuilt_after_groups']=refresh_file_type_rows(img)
        owners[FT_ROWS-BASE]=SHARED_VIDEO
    detail['routes']=_emit_v051_routes(routes,put)
    detail['strings']=_emit_v051_text(img,strings,put)
    detail['menu']=_emit_video(img,menu,mask,callbacks,put,code_builder=_shared_video_code_v051,config=V051_VIDEO_CONFIG)
    # A Log-only video menu has no condition or resource-accessor ingress.
    return _projection(stock,mask,img,owners,detail,compiled_sha)


def compose_recipe_v051(stock,mask,photo=None):
    """Project the pinned subset recipe before compacting its shared text bank."""
    from z8_feature_suite_v051 import run_isolated

    return run_isolated('z8_wlm_native_recipes:_compose_recipe_v051',stock,mask,photo)


def component_windows(recipe,owner):
    """Filter selected recipes for a component/shared-library package.

    The MOV close site is delegated to the one shared bridge; loader reserve
    is a foundation prerequisite and is never a runtime package record.
    """
    if owner not in (*COMPONENTS,SHARED_LOCALE,SHARED_VIDEO,SHARED_QUALITY):raise ValueError('unknown native owner')
    return tuple(w for w in recipe.windows if w.owner==owner and w.address not in (COPY_END_SITE,MOV_CLOSE))
