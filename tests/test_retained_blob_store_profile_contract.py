from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_retained_sd_admission_uses_only_the_immutable_profile_authority():
    source = read("main/storage/retained_blob_store.c")

    assert '#include "app/release_profile.h"' in source
    assert "static bool release_profile_allows_sd_history(void)" in source
    assert (
        "d1l_release_feature_available(\n"
        "        D1L_RELEASE_FEATURE_SD_HISTORY)"
    ) in source
    assert (
        "release_profile_allows_sd_history() &&\n"
        "        data_ready"
    ) in source
    assert "profile_allows_sd && s_store_sd_enabled[config->id]" in source
    assert "d1l_release_profile_set" not in source
    assert "d1l_release_sd_history_mode_set" not in source


def test_retained_store_has_no_sd_format_path():
    retained = (
        read("main/storage/retained_blob_store.c")
        + read("main/storage/retained_blob_store.h")
    ).lower()

    for forbidden in (
        "d1l_rp2040_bridge_sd_format",
        "d1l_rp2040_req_format",
        "format-confirm",
        "storage sd format",
    ):
        assert forbidden not in retained
