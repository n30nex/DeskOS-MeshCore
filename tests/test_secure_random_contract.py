from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(relative: str) -> str:
    return (ROOT / relative).read_text(encoding="utf-8")


def function(source: str, signature: str, next_signature: str) -> str:
    return source.split(signature, 1)[1].split(next_signature, 1)[0]


def test_secure_random_is_seeded_before_every_app_subsystem():
    app = read("main/app_main.c")
    cmake = read("main/CMakeLists.txt")
    init = app.index("d1l_secure_random_init()")
    for later in (
        "nvs_flash_init()",
        "d1l_retained_blob_store_init()",
        "d1l_storage_status_init()",
        "d1l_rp2040_bridge_init()",
        "d1l_meshcore_service_init()",
        "d1l_board_init()",
        "d1l_connectivity_init()",
        "d1l_ui_phase1_start()",
    ):
        assert init < app.index(later)
    assert '"platform/secure_random.c"' in cmake
    assert "bootloader_support" in cmake
    assert '\\"secure_random_ready\\":%s' in app
    assert '\\"secure_random_error\\":\\"%s\\"' in app


def test_hardware_entropy_is_early_only_and_late_generation_fails_closed():
    source = read("main/platform/secure_random.c")
    init = function(
        source, "esp_err_t d1l_secure_random_init(void)",
        "esp_err_t d1l_secure_random_fill(void *dest, size_t length)",
    )
    fill = function(
        source, "esp_err_t d1l_secure_random_fill(void *dest, size_t length)",
        "bool d1l_secure_random_ready(void)",
    )
    assert init.index("bootloader_random_enable()") < init.index(
        "esp_fill_random(s_seed_source.bytes"
    ) < init.index("bootloader_random_disable()")
    assert init.index("bootloader_random_disable()") < init.index(
        "mbedtls_ctr_drbg_seed("
    )
    assert "mbedtls_ctr_drbg_set_reseed_interval(&s_drbg, INT_MAX - 1)" in init
    assert "s_init_attempted" in init
    assert "bootloader_random" not in fill
    assert "esp_fill_random" not in fill
    assert "mbedtls_ctr_drbg_random" in fill
    assert "xSemaphoreTake" in fill
    assert "clear_sensitive_bytes(dest, length)" in fill
    assert "s_ready = false" in fill
    assert "s_status = ESP_FAIL" in fill
    under_lock_recheck = fill.index("if (!s_ready)", fill.index("xSemaphoreTake"))
    assert fill.index("xSemaphoreTake") < under_lock_recheck < fill.index(
        "mbedtls_ctr_drbg_random"
    )
    under_lock_failure = fill[under_lock_recheck : fill.index(
        "mbedtls_ctr_drbg_random"
    )]
    assert "xSemaphoreGive" in under_lock_failure
    assert "clear_sensitive_bytes(dest, length)" in under_lock_failure


def test_identity_and_channel_secrets_use_only_the_secure_service():
    app_model = read("main/app/app_model.c")
    mesh = read("main/mesh/meshcore_service.c")
    create = function(
        app_model, "esp_err_t d1l_app_model_create_channel(",
        "esp_err_t d1l_app_model_import_channel_uri(",
    )
    identity = function(
        mesh, "esp_err_t d1l_meshcore_service_ensure_identity(void)",
        "d1l_meshcore_service_status_t d1l_meshcore_service_status(void)",
    )
    for body in (create, identity):
        assert "d1l_secure_random_fill(" in body
        assert "esp_fill_random" not in body
    assert "return secret_ret" in create
    assert "return random_ret" in identity
    assert create.index("d1l_secure_random_fill(") < create.index(
        "d1l_channel_store_add("
    )
    assert identity.index("d1l_secure_random_fill(") < identity.index(
        "ed25519_create_keypair("
    )
    assert "secure_zero_bytes(seed, sizeof(seed))" in identity
    assert "memset(seed, 0, sizeof(seed))" not in identity
