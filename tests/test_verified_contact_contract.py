from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def _verified_upsert_body() -> str:
    source = (ROOT / "main/mesh/contact_store.c").read_text(encoding="utf-8")
    start = source.index("esp_err_t d1l_contact_store_upsert_verified_advert")
    end = source.index("esp_err_t d1l_contact_store_import_uri", start)
    return source[start:end]


def test_verified_advert_upsert_is_full_key_authoritative_and_non_evicting():
    body = _verified_upsert_body()

    assert "find_unique_index_by_public_key_hex" in body
    assert "find_unique_index_by_fingerprint_hex" in body
    assert "D1L_CONTACT_VERIFIED_ADVERT_COLLISION" in body
    assert "D1L_CONTACT_VERIFIED_ADVERT_FULL" in body
    assert "oldest_index(" not in body
    assert "s_dropped_oldest++" not in body


def test_verified_advert_upsert_preserves_user_preferences():
    body = _verified_upsert_body()

    for user_preference in (
        "entry->favorite =",
        "entry->muted =",
        "entry->out_path_valid =",
        "entry->out_path_len =",
        "entry->out_path_updated_ms =",
    ):
        assert user_preference not in body


def test_verified_advert_upsert_publishes_before_coalesced_persistence():
    body = _verified_upsert_body()

    assert "mark_deferred_persistence_locked(now_ms);" in body
    assert "persist_store_or_rollback" not in body
    publish = body.index("mark_deferred_persistence_locked(now_ms);")
    assert body.index("*out_result = result;", publish) < body.index(
        "d1l_store_lock_give(&s_store_lock);", publish
    )


def test_signed_advert_rx_binds_exact_full_key_to_truthful_retained_contact_result():
    service = (ROOT / "main/mesh/meshcore_service.c").read_text(encoding="utf-8")
    receiver = service.split("static void parse_rx_advert_packet", 1)[1].split(
        "static void on_tx_done", 1
    )[0]

    signature = receiver.index("verify_advert_signature")
    self_filter = receiver.index("memcmp(pub_key, settings->identity_public_key")
    admission = receiver.index("d1l_meshcore_advert_admit_verified")
    outcome_dispatch = receiver.index("switch (admission.outcome)")
    accepted = receiver.index("s_status.rx_adverts++")
    assert signature < self_filter < admission < outcome_dispatch < accepted

    assert "D1L_CONTACT_VERIFIED_ADVERT_CREATED" in service
    assert "D1L_CONTACT_VERIFIED_ADVERT_UPDATED" in service
    assert "D1L_CONTACT_VERIFIED_ADVERT_PROMOTED_PLACEHOLDER" in service
    assert "D1L_CONTACT_VERIFIED_ADVERT_FULL" in service
    assert 'return "new";' in service
    assert 'return "updated";' in service
    assert 'return "promoted";' in service
    assert 'return "collision";' in service
    assert 'return "full";' in service
    assert "is_new" not in receiver
    assert "d1l_node_store_upsert_advert" not in receiver
    assert "d1l_contact_store_upsert_verified_advert" not in receiver
    for outcome in (
        "D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_SUCCEEDED",
        "D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_FAILED",
        "D1L_MESHCORE_ADVERT_ADMISSION_REPLAY_REJECTED",
        "D1L_MESHCORE_ADVERT_ADMISSION_KEY_COLLISION",
        "D1L_MESHCORE_ADVERT_ADMISSION_NODE_STORE_ERROR",
        "D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED",
    ):
        assert outcome in receiver
    for event in (
        '"advert_contact_retry"',
        '"advert_contact_retry_error"',
        '"advert_replay"',
        '"advert_key_collision"',
        '"advert_store_error"',
    ):
        assert event in receiver


def test_admission_owns_replay_recovery_and_exact_full_key_contact_promotion():
    admission = (ROOT / "main/mesh/meshcore_advert_admission.c").read_text(
        encoding="utf-8"
    )
    body = admission.split("esp_err_t d1l_meshcore_advert_admit_verified", 1)[1]

    node_upsert = body.index("d1l_node_store_upsert_advert")
    stale_filter = body.index("if (stale)")
    retained_readback = body.index("d1l_node_store_find_by_fingerprint")
    retained_timestamp = body.index("retained_node.advert_timestamp == advert_timestamp")
    retained_key = body.index(
        "strcmp(retained_node.public_key_hex, public_key_hex) == 0"
    )
    contact_absent = body.index("!d1l_contact_store_find_by_public_key")
    retry_upsert = body.index("d1l_contact_store_upsert_verified_advert")
    node_error = body.index("if (node_ret != ESP_OK)")
    accepted_readback = body.index(
        "d1l_node_store_find_by_fingerprint", retained_readback + 1
    )
    accepted_upsert = body.index(
        "d1l_contact_store_upsert_verified_advert", retry_upsert + 1
    )
    accepted_outcome = body.index(
        "D1L_MESHCORE_ADVERT_ADMISSION_ACCEPTED", accepted_upsert
    )
    assert (
        node_upsert
        < stale_filter
        < retained_readback
        < retained_timestamp
        < retained_key
        < contact_absent
        < retry_upsert
        < node_error
        < accepted_readback
        < accepted_upsert
        < accepted_outcome
    )
    assert "D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_SUCCEEDED" in body
    assert "D1L_MESHCORE_ADVERT_ADMISSION_CONTACT_RETRY_FAILED" in body
    assert "D1L_MESHCORE_ADVERT_ADMISSION_REPLAY_REJECTED" in body
    assert "D1L_MESHCORE_ADVERT_ADMISSION_KEY_COLLISION" in body


def test_heard_node_rejects_full_key_prefix_collision_before_replay_or_mutation():
    source = (ROOT / "main/mesh/node_store.c").read_text(encoding="utf-8")
    upsert = source.split("esp_err_t d1l_node_store_upsert_advert", 1)[1].split(
        "d1l_node_store_stats_t d1l_node_store_stats", 1
    )[0]

    collision = upsert.index("!public_keys_equal")
    replay = upsert.index("d1l_meshcore_lifetime_advert_is_strictly_newer(")
    mutation = upsert.index("entry->seq =")
    assert collision < replay < mutation
    assert "return ESP_ERR_INVALID_STATE;" in upsert[collision:replay]
