"""Pinned native control ABI for the complete upstream framing backend.

This verifies original C3.11 entry bytes. It does not install the backend: the
feature-suite composer must validate the complete pinned upstream overlay.
"""
import hashlib
MAIN_BYTES=81807709
MAIN_SHA256='2b43a31c9de838f84b12416de4348a7004e65be07f853ec56ec2b10f3a8e3853'
UPSTREAM_COMMIT='a21fc8c04b6dd863439c4192d387b755633287f6'
V051_UPSTREAM_COMMIT='25fefa1e17f321ee285bea77b3c54427f9912e4b'
BASE=0x40160000
PREIMAGES={
    0x403ec39c:'8da73314',
    0x410d5ec8:'f37bbfa9f303002aecffff972001003621148bd2c181a8f2',
    0x410d61d0:'f60f1df8f55301a9f37b02a916b98ad2d68fa0f25601c0f21600e0f2f303002ae00316aae14f01291f59f19715688dd2',
}
V051_PREIMAGES={
    0x403a036c:'f963bca9',
    0x4056cde4:'c9a98552',
    0x407138d0:'f75bbda9',
    0x41055ac0:'b53d0094',
    0x41055b38:'62010054',
    0x410567dc:'6e3a0094',
    0x4105693c:'e2010054',
    0x40cf4620:'0d008352eb031faa0f228252cc010c8bad02a072',
}

def verify_original(original, *, suite='legacy'):
    if suite not in ('legacy','v051'):
        raise ValueError('unknown framing suite')
    if len(original)!=MAIN_BYTES or hashlib.sha256(original).hexdigest()!=MAIN_SHA256:
        raise ValueError('framing controls require exact original Z8 C3.11 MAIN')
    preimages={**PREIMAGES,**V051_PREIMAGES} if suite=='v051' else PREIMAGES
    for address,expected in preimages.items():
        if original[address-BASE:address-BASE+len(bytes.fromhex(expected))]!=bytes.fromhex(expected):
            raise ValueError('framing native control preimage differs: '+hex(address))
    profile=dict(abi=1,upstream_commit=V051_UPSTREAM_COMMIT if suite=='v051' else UPSTREAM_COMMIT,
        service='camera.video.framing',native_set='0x403ec39c',native_set_body='0x410d61d0',
        native_get='0x410d5ec8',nraw_property='0x05001261',rate_property='0x0500125b',
        nraw_mirror='0xa03d5c290',nraw_native_order=['Normal','High','Fine','Basic'],
        desqueeze_bank_byte='0xa03d5c0ad',open_gate_bank_byte='0xa03d5c02b',
        desqueeze=['Off','1.25x','1.30x','1.33x (4/3)','1.50x','1.60x','1.80x','2.00x'],
        open_gate_selection='native Frame size/rate transaction only',
        depends_on_complete_upstream_overlay=True,physical_qualified=False,
        native_entry_preimages={hex(a):v for a,v in preimages.items()})
    if suite=='v051':
        profile.update(open_gate_selection='native Video file type and Frame size/rate transactions only',
            file_type_control='0x35ca',file_type_value=6,file_type_bank_word='0xa03d5c014',
            frame_size_bank_word='0xa03d5c274',frame_rate_bank_word='0xa03d5c27c',
            alternate_size_rate_bank_halfword='0xa03d5c0ae',
            open_gate_modes=[dict(width=width,height=height,fps=fps,size=size,rate=rate)
                for width,height,size,rates in ((8256,5504,6,((4,30),(5,25),(6,24))),
                    (4128,2752,4,tuple(enumerate((120,100,60,50,30,25,24)))))
                for rate,fps in rates])
    return profile
