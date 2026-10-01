import json

import pytest

from bfval import fixtures


def test_templates_cover_validation_set_and_match_files():
    t = fixtures.fixture_table()
    expect = {"B1", "B2", "B3", "B4", "B5", "B7", "B8", "B16", "SSN-L", "SL5", "SL7", "SL9", "PINK",
              "H25", "H50", "H75", "NAT", "BAL", "DEN"}
    assert expect <= set(t)
    for name, sc in t.items():
        on_disk = json.loads((fixtures.DEFAULT_SCENARIO_DIR / f"{name}.json").read_text())
        assert on_disk == sc, f"{name}.json is stale; run python -m bfval.fixtures write-templates"
        assert sc["schema"] == "babbleforge.scenario" and sc["seed"] in fixtures.SEEDS
    assert t["B4"]["laboratory"]["talkers"] == 4 and t["B4"]["laboratory"]["maxGapMs"] == 100
    assert t["H25"]["laboratory"]["babbleFraction"] == 0.25
    assert t["NAT"]["durationS"] == 600 and "laboratory" not in t["NAT"]
    assert fixtures.SEEDS == [1001, 1002, 1003, 1004, 1005]


def test_tune_proposes_and_clamps(tmp_path):
    import tune
    data = tmp_path / "data" / "areas"
    data.mkdir(parents=True)
    (data / "office.json").write_text(json.dumps({"talkers": {"meanActive": 6.5}, "spectrum": {"lfTrimDb125": -1.5},
                                                  "evidenceLabel": "engineering-default-requires-validation"}))
    csv_path = tmp_path / "r.csv"
    csv_path.write_text("condition,metric,observed,target\narea:office,meanActive,6.0,6.5\n"
                        "area:office,meanActive,6.0,6.5\narea:office,octave125DevDb,20,0\n"
                        "area:office,unknown,1,2\narea:nope,meanActive,1,2\n")
    out = tmp_path / "out"
    assert tune.main([str(csv_path), "--data-dir", str(tmp_path / "data"), "--out-dir", str(out),
                      "--study-id", "S1", "--apply-label"]) == 0
    doc = json.loads((out / "proposed" / "areas" / "office.json").read_text())
    assert doc["talkers"]["meanActive"] == pytest.approx(7.0)
    assert doc["spectrum"]["lfTrimDb125"] == -6.0  # clamped
    assert doc["evidenceLabel"] == "validated:S1" and "S1" in doc["evidenceRefs"]
    md = (out / "tuning_diff.md").read_text()
    assert "talkers.meanActive" in md and "no mapping" in md
    # source untouched
    assert json.loads((data / "office.json").read_text())["talkers"]["meanActive"] == 6.5
