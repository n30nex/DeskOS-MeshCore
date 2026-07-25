from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def test_d1l_uses_custom_8mb_partition_table():
    defaults = read("sdkconfig.defaults")
    table = read("partitions_d1l.csv")

    assert "CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y" in defaults
    assert "CONFIG_PARTITION_TABLE_CUSTOM=y" in defaults
    assert 'CONFIG_PARTITION_TABLE_FILENAME="partitions_d1l.csv"' in defaults

    assert "nvs,        data, nvs,     0x9000,   0x6000," in table
    assert "otadata,    data, ota,     0xf000,   0x2000," in table
    assert "phy_init,   data, phy,     0x11000,  0x1000," in table
    assert "ota_0,      app,  ota_0,   0x20000,  0x3E0000," in table
    assert "ota_1,      app,  ota_1,   0x400000, 0x3E0000," in table
    assert "d1l_ret_meta,data, 0x40,    0x7E0000, 0x1000," in table
    assert "d1l_retained,data, nvs,     0x7E1000, 0x1F000," in table


def test_d1l_dual_ota_partitions_have_release_headroom():
    table = read("partitions_d1l.csv")
    ota_0 = [
        part.strip()
        for part in next(
            line for line in table.splitlines() if line.startswith("ota_0,")
        ).split(",")
    ]
    ota_1 = [
        part.strip()
        for part in next(
            line for line in table.splitlines() if line.startswith("ota_1,")
        ).split(",")
    ]

    assert ota_0[:5] == ["ota_0", "app", "ota_0", "0x20000", "0x3E0000"]
    assert ota_1[:5] == ["ota_1", "app", "ota_1", "0x400000", "0x3E0000"]
    ota_0_offset = int(ota_0[3], 16)
    ota_0_size = int(ota_0[4], 16)
    ota_1_offset = int(ota_1[3], 16)
    ota_1_size = int(ota_1[4], 16)

    assert ota_0_offset == 0x20000
    assert ota_0_size >= 0x200000
    assert ota_0_offset + ota_0_size == ota_1_offset
    assert ota_1_size >= 0x200000
    assert ota_1_offset + ota_1_size == 0x7E0000

    meta_line = next(
        line for line in table.splitlines() if line.startswith("d1l_ret_meta,")
    )
    meta = [part.strip() for part in meta_line.split(",")]
    assert meta[:5] == ["d1l_ret_meta", "data", "0x40", "0x7E0000", "0x1000"]

    retained_line = next(
        line for line in table.splitlines() if line.startswith("d1l_retained,")
    )
    retained = [part.strip() for part in retained_line.split(",")]
    assert retained[:5] == ["d1l_retained", "data", "nvs", "0x7E1000", "0x1F000"]
    assert int(meta[3], 16) == ota_1_offset + ota_1_size
    assert int(meta[3], 16) + int(meta[4], 16) == int(retained[3], 16)
    assert int(retained[3], 16) + int(retained[4], 16) == 0x800000
