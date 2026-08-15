from pathlib import Path
import hashlib


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def test_hashtag_channels_use_the_meshcore_standard_derivation_and_clear_keys():
    app = read("main/app/app_model.c")
    ui = read("main/ui/ui_channel_sheets.c")

    assert hashlib.sha256(b"#test").digest()[:16].hex() == (
        "9cd8fcf22a47333b591d96a2b848b73f"
    )
    assert "name && name[0] == '#'" in app
    assert "mbedtls_md_info_from_type(MBEDTLS_MD_SHA256)" in app
    assert "memcpy(secret, digest, sizeof(secret))" in app
    assert "clear_sensitive_bytes(digest, sizeof(digest))" in app
    assert "clear_sensitive_bytes(secret, sizeof(secret))" in app
    assert '"Start with # to join a shared hashtag; other names stay private."' in ui
    assert '"#yyc or private name"' in ui


def test_channel_rx_uses_trusted_arrival_time_when_wire_time_is_bad():
    service = read("main/mesh/meshcore_service.c")
    helper = service.split(
        "static uint32_t channel_message_display_timestamp", 1
    )[1].split("static d1l_channel_rx_store_result_t", 1)[0]

    assert "plausible_epoch_floor" in helper
    assert "d1l_time_service_status(&time_status)" in helper
    assert "time_status.display_time_valid" in helper
    assert "return local_timestamp" in helper
    assert "channel_message_display_timestamp(read_le32(plain))" in service
