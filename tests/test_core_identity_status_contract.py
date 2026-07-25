from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_identity_status_emits_full_retained_public_key_and_fingerprint():
    source = (ROOT / "main/comms/usb_console.c").read_text(encoding="utf-8")
    block = source.split("static void cmd_identity_status(void)", 1)[1].split(
        "static void cmd_i2c(void)", 1
    )[0]

    assert 'ok_begin("identity status")' in block
    assert '\\"public_key_ready\\":%s,\\"public_key\\":' in block
    assert (
        "print_hex_bytes_json(settings->identity_public_key,"
        in block
    )
    assert "sizeof(settings->identity_public_key)" in block
    assert '\\"fingerprint\\":\\"%s\\"' in block
