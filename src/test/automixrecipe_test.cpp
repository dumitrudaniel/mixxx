#include "mixer/automixrecipe.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

const AutomixLane* findLane(
        const AutomixRecipe& recipe, AutomixDeckRole deck, AutomixParam param) {
    for (const AutomixLane& lane : recipe.lanes) {
        if (lane.deck == deck && lane.param == param) {
            return &lane;
        }
    }
    return nullptr;
}

// Minimal valid file with one recipe whose lanes are given as JSON text.
QByteArray recipeFile(const char* lanesJson, const char* extraRecipeFields = "") {
    return QByteArray(R"({"version": 1, "settings": {"arm_quantum_bars": 4},
        "recipes": [{"id": "t", "length_bars": 2, )") +
            extraRecipeFields + R"( "lanes": [)" + lanesJson + "]}]}";
}

} // namespace

class AutomixRecipeTest : public testing::Test {
};

TEST_F(AutomixRecipeTest, BuiltinParsesWithAllSelectorRecipes) {
    AutomixRecipeBook book;
    QString error;
    ASSERT_TRUE(AutomixRecipeBook::parse(AutomixRecipeBook::builtinJson(), &book, &error))
            << error.toStdString();
    EXPECT_EQ(5, book.size());
    for (const QString& id : AutomixRecipeBook::selectorIds()) {
        EXPECT_NE(nullptr, book.find(id)) << id.toStdString();
    }
    EXPECT_DOUBLE_EQ(1.0, book.armQuantumBars());
    EXPECT_DOUBLE_EQ(1.0, book.resumeGlideBeats());
    EXPECT_EQ(5, AutomixRecipeBook::builtin().size());
}

TEST_F(AutomixRecipeTest, DefaultSelectorSlotIsStandard8) {
    const AutomixRecipe* pRecipe = AutomixRecipeBook::builtin().forSelectorIndex(
            AutomixRecipeBook::kDefaultSelectorIndex);
    ASSERT_NE(nullptr, pRecipe);
    EXPECT_EQ(QStringLiteral("standard8"), pRecipe->id);
    EXPECT_DOUBLE_EQ(32.0, pRecipe->lengthBeats);
    // Filter and volume lanes ship disabled: six EQ lanes remain.
    EXPECT_EQ(6u, pRecipe->lanes.size());
    EXPECT_TRUE(pRecipe->tempoMatch);
    EXPECT_TRUE(pRecipe->autoPlayIncoming);
    EXPECT_EQ(3u, pRecipe->prepareIncoming.size());
}

TEST_F(AutomixRecipeTest, Standard8FollowsThePlan) {
    const AutomixRecipe& r = *AutomixRecipeBook::builtin().find(QStringLiteral("standard8"));
    const AutomixLane* inMid = findLane(r, AutomixDeckRole::Incoming, AutomixParam::EqMid);
    const AutomixLane* inHigh = findLane(r, AutomixDeckRole::Incoming, AutomixParam::EqHigh);
    const AutomixLane* inLow = findLane(r, AutomixDeckRole::Incoming, AutomixParam::EqLow);
    const AutomixLane* outLow = findLane(r, AutomixDeckRole::Outgoing, AutomixParam::EqLow);
    const AutomixLane* outHigh = findLane(r, AutomixDeckRole::Outgoing, AutomixParam::EqHigh);
    const AutomixLane* outMid = findLane(r, AutomixDeckRole::Outgoing, AutomixParam::EqMid);
    ASSERT_TRUE(inMid && inHigh && inLow && outLow && outHigh && outMid);
    EXPECT_EQ(nullptr, findLane(r, AutomixDeckRole::Outgoing, AutomixParam::Filter));

    // Phase 1 (bars 0-4): incoming mids rise from kill, bass stays cut,
    // highs only reach -12 dB; outgoing untouched.
    EXPECT_DOUBLE_EQ(0.0, inMid->valueAt(0.0, 0.0));
    EXPECT_NEAR(1.0, inMid->valueAt(16.0, 0.0), 1e-12);
    EXPECT_DOUBLE_EQ(0.0, inLow->valueAt(15.9, 0.0));
    EXPECT_NEAR(0.2512, inHigh->valueAt(16.0, 0.0), 1e-4);
    EXPECT_DOUBLE_EQ(1.0, outLow->valueAt(15.9, 1.0));
    EXPECT_DOUBLE_EQ(1.0, outMid->valueAt(15.9, 1.0));

    // Phase 2 (downbeat of bar 4): bass swaps in one beat, highs in one bar.
    EXPECT_NEAR(0.0, outLow->valueAt(17.0, 1.0), 1e-12);
    EXPECT_NEAR(1.0, inLow->valueAt(17.0, 0.0), 1e-12);
    EXPECT_GT(outHigh->valueAt(17.0, 1.0), 0.0);
    EXPECT_NEAR(0.0, outHigh->valueAt(20.0, 1.0), 1e-12);
    EXPECT_NEAR(1.0, inHigh->valueAt(20.0, 0.0), 1e-12);

    // Phase 3 (bars 4-5.5): outgoing mids fall to kill; from beat 22 (the
    // last third) only the incoming track plays (Dan, 2026-10-08).
    EXPECT_GT(outMid->valueAt(17.0, 1.0), 0.9);
    EXPECT_NEAR(0.5, outMid->valueAt(19.0, 1.0), 1e-12);
    EXPECT_NEAR(0.0, outMid->valueAt(22.0, 1.0), 1e-12);
    EXPECT_NEAR(0.0, outMid->valueAt(32.0, 1.0), 1e-12);
}

TEST_F(AutomixRecipeTest, OutgoingIsOutBeforeTheLastThird) {
    // Every beatmatched built-in recipe kills all outgoing lanes by 11/16 of
    // its length (Standard 8: beat 22) and has the incoming EQ at unity there.
    // Fade curat has no beatmatched overlap and keeps its own 2-bar fade.
    for (const QString& id : AutomixRecipeBook::selectorIds()) {
        const AutomixRecipe* pRecipe = AutomixRecipeBook::builtin().find(id);
        ASSERT_NE(nullptr, pRecipe) << id.toStdString();
        if (!pRecipe->tempoMatch) {
            continue;
        }
        const double outBeat = pRecipe->lengthBeats * 11.0 / 16.0;
        const double swapBeat = pRecipe->lengthBeats / 2.0;
        for (const AutomixLane& lane : pRecipe->lanes) {
            if (lane.deck == AutomixDeckRole::Outgoing) {
                EXPECT_NEAR(0.0, lane.valueAt(outBeat, 1.0), 1e-12) << id.toStdString();
                // Untouched until the swap: the outgoing voice finishes its phrase.
                EXPECT_DOUBLE_EQ(1.0, lane.valueAt(swapBeat, 1.0)) << id.toStdString();
            } else {
                EXPECT_NEAR(1.0, lane.valueAt(outBeat, 0.0), 1e-12) << id.toStdString();
            }
        }
    }
}

TEST_F(AutomixRecipeTest, Standard8BassSwapIsEqualPower) {
    const AutomixRecipe& r = *AutomixRecipeBook::builtin().find(QStringLiteral("standard8"));
    const AutomixLane* inLow = findLane(r, AutomixDeckRole::Incoming, AutomixParam::EqLow);
    const AutomixLane* outLow = findLane(r, AutomixDeckRole::Outgoing, AutomixParam::EqLow);
    for (int i = 0; i <= 10; ++i) {
        const double beat = 16.0 + i / 10.0;
        const double out = outLow->valueAt(beat, 1.0);
        const double in = inLow->valueAt(beat, 0.0);
        EXPECT_NEAR(1.0, out * out + in * in, 1e-9) << "beat=" << beat;
    }
}

TEST_F(AutomixRecipeTest, LanesRampFromTheKnobsCurrentValue) {
    const AutomixRecipe& r = *AutomixRecipeBook::builtin().find(QStringLiteral("standard8"));
    const AutomixLane* outLow = findLane(r, AutomixDeckRole::Outgoing, AutomixParam::EqLow);
    const AutomixLane* inMid = findLane(r, AutomixDeckRole::Incoming, AutomixParam::EqMid);
    // Dan left the outgoing bass at 0.8: it holds there, no jump to 1.0.
    EXPECT_DOUBLE_EQ(0.8, outLow->valueAt(0.0, 0.8));
    EXPECT_DOUBLE_EQ(0.8, outLow->valueAt(10.0, 0.8));
    // Incoming mids already half up: the ramp starts from 0.5.
    EXPECT_DOUBLE_EQ(0.5, inMid->valueAt(0.0, 0.5));
    EXPECT_GT(inMid->valueAt(4.0, 0.5), 0.5);
}

TEST_F(AutomixRecipeTest, FadeCuratSkipsTempoMatchAndStartsIncomingLate) {
    const AutomixRecipe& r = *AutomixRecipeBook::builtin().find(QStringLiteral("fade_curat"));
    EXPECT_FALSE(r.tempoMatch);
    EXPECT_EQ(AutomixTempoMode::Off, r.tempo.mode);
    EXPECT_DOUBLE_EQ(6.0, r.incomingPlayAtBeat);
    EXPECT_DOUBLE_EQ(8.0, r.lengthBeats);
}

TEST_F(AutomixRecipeTest, BeatmatchedRecipesMeetInTheMiddleThenReturn) {
    // Dan's decision #6 (docs/decisions/0027): every beatmatched built-in
    // recipe meets at the midpoint tempo in 4 bars and returns in 8.
    for (const QString& id : AutomixRecipeBook::selectorIds()) {
        const AutomixRecipe& r = *AutomixRecipeBook::builtin().find(id);
        if (id == QStringLiteral("fade_curat")) {
            continue;
        }
        EXPECT_EQ(AutomixTempoMode::MeetReturn, r.tempo.mode) << id.toStdString();
        EXPECT_TRUE(r.tempoMatch) << id.toStdString();
        EXPECT_DOUBLE_EQ(16.0, r.tempo.meetBeats) << id.toStdString();
        EXPECT_DOUBLE_EQ(32.0, r.tempo.returnBeats) << id.toStdString();
    }
}

TEST_F(AutomixRecipeTest, TempoOptionDefaultsAndLegacyTempoMatch) {
    const auto parseTempo = [](const char* fields, AutomixRecipe* pRecipe) {
        AutomixRecipeBook book;
        QString error;
        const bool ok = AutomixRecipeBook::parse(recipeFile("", fields), &book, &error);
        EXPECT_TRUE(ok) << fields << " " << error.toStdString();
        if (ok) {
            *pRecipe = *book.find(QStringLiteral("t"));
        }
    };
    AutomixRecipe r;
    // Absent: meet_return with defaults (files written before ADR 0027).
    parseTempo("", &r);
    EXPECT_EQ(AutomixTempoMode::MeetReturn, r.tempo.mode);
    EXPECT_TRUE(r.tempoMatch);
    EXPECT_DOUBLE_EQ(16.0, r.tempo.meetBeats);
    EXPECT_DOUBLE_EQ(32.0, r.tempo.returnBeats);
    // Legacy "tempo_match": false = no tempo change.
    parseTempo(R"("tempo_match": false,)", &r);
    EXPECT_EQ(AutomixTempoMode::Off, r.tempo.mode);
    EXPECT_FALSE(r.tempoMatch);
    parseTempo(R"("tempo": {"mode": "match_incoming"},)", &r);
    EXPECT_EQ(AutomixTempoMode::MatchIncoming, r.tempo.mode);
    EXPECT_TRUE(r.tempoMatch);
    parseTempo(R"("tempo": {"meet_bars": 2, "return_bars": 0}, "tempo_match": true,)", &r);
    EXPECT_EQ(AutomixTempoMode::MeetReturn, r.tempo.mode);
    EXPECT_DOUBLE_EQ(8.0, r.tempo.meetBeats);
    EXPECT_DOUBLE_EQ(0.0, r.tempo.returnBeats);
    parseTempo(R"("tempo": {"mode": "off"}, "tempo_match": false,)", &r);
    EXPECT_EQ(AutomixTempoMode::Off, r.tempo.mode);
    EXPECT_FALSE(r.tempoMatch);
}

TEST_F(AutomixRecipeTest, TempoOptionRejectsBrokenValues) {
    for (const char* bad : {R"("tempo": {"mode": "sync"},)",
                 R"("tempo": {"meet_bars": 0},)",
                 R"("tempo": {"return_bars": -1},)",
                 R"("tempo": "meet_return",)",
                 R"("tempo": {"mode": "meet_return"}, "tempo_match": false,)",
                 R"("tempo": {"mode": "off"}, "tempo_match": true,)",
                 R"("tempo_match": "no",)"}) {
        AutomixRecipeBook book = AutomixRecipeBook::builtin();
        QString error;
        EXPECT_FALSE(AutomixRecipeBook::parse(recipeFile("", bad), &book, &error)) << bad;
        EXPECT_FALSE(error.isEmpty()) << bad;
    }
}

TEST_F(AutomixRecipeTest, ValueAtHoldsOutsideThePoints) {
    AutomixRecipeBook book;
    QString error;
    ASSERT_TRUE(AutomixRecipeBook::parse(recipeFile(R"(
        {"deck": "outgoing", "param": "eq_mid", "points": [
            {"beat": 2, "value": 0.5, "shape": "linear"},
            {"beat": 6, "value": 0.1}]})"),
            &book,
            &error))
            << error.toStdString();
    const AutomixLane& lane = book.find(QStringLiteral("t"))->lanes.front();
    // An implicit "current" point at beat 0 is prepended.
    ASSERT_EQ(3u, lane.points.size());
    EXPECT_TRUE(lane.points.front().useStartValue);
    EXPECT_DOUBLE_EQ(0.9, lane.valueAt(-1.0, 0.9));
    EXPECT_DOUBLE_EQ(0.7, lane.valueAt(1.0, 0.9));
    EXPECT_DOUBLE_EQ(0.3, lane.valueAt(4.0, 0.9));
    EXPECT_DOUBLE_EQ(0.1, lane.valueAt(6.0, 0.9));
    EXPECT_DOUBLE_EQ(0.1, lane.valueAt(99.0, 0.9));
}

TEST_F(AutomixRecipeTest, PointsOnTheSameBeatJump) {
    AutomixRecipeBook book;
    QString error;
    ASSERT_TRUE(AutomixRecipeBook::parse(recipeFile(R"(
        {"deck": "incoming", "param": "eq_low", "points": [
            {"beat": 0, "value": "kill"},
            {"beat": 4, "value": "kill"},
            {"beat": 4, "value": "unity"}]})"),
            &book,
            &error))
            << error.toStdString();
    const AutomixLane& lane = book.find(QStringLiteral("t"))->lanes.front();
    EXPECT_DOUBLE_EQ(0.0, lane.valueAt(3.99, 0.5));
    EXPECT_DOUBLE_EQ(1.0, lane.valueAt(4.0, 0.5));
}

TEST_F(AutomixRecipeTest, ParsesNamedAndDecibelValues) {
    AutomixRecipeBook book;
    QString error;
    ASSERT_TRUE(AutomixRecipeBook::parse(recipeFile(R"(
        {"deck": "incoming", "param": "eq_high", "points": [
            {"bar": 0, "value": "-6 dB"}, {"bar": 1, "value": "+3dB"}]},
        {"deck": "outgoing", "param": "filter", "points": [
            {"bar": 0, "value": "neutral"}, {"bar": 2, "value": 0.75, "shape": "smoothstep"}]})",
                                                    R"("prepare_incoming": {"eq_low": "kill"},)"),
            &book,
            &error))
            << error.toStdString();
    const AutomixRecipe& r = *book.find(QStringLiteral("t"));
    EXPECT_NEAR(0.5012, r.lanes[0].points[0].value, 1e-4);
    EXPECT_NEAR(1.4125, r.lanes[0].points[1].value, 1e-4);
    EXPECT_DOUBLE_EQ(0.5, r.lanes[1].points[0].value);
    ASSERT_EQ(1u, r.prepareIncoming.size());
    EXPECT_DOUBLE_EQ(0.0, r.prepareIncoming[0].value);
    EXPECT_DOUBLE_EQ(4.0, book.armQuantumBars());
}

TEST_F(AutomixRecipeTest, DisabledLanesAreDropped) {
    AutomixRecipeBook book;
    QString error;
    ASSERT_TRUE(AutomixRecipeBook::parse(recipeFile(R"(
        {"deck": "incoming", "param": "volume", "enabled": false, "points": [
            {"bar": 0, "value": "-2dB"}, {"bar": 1, "value": "current"}]})"),
            &book,
            &error))
            << error.toStdString();
    EXPECT_TRUE(book.find(QStringLiteral("t"))->lanes.empty());
}

TEST_F(AutomixRecipeTest, RejectsBrokenFilesAndKeepsTheOldBook) {
    const QList<QByteArray> broken = {
            QByteArray("{not json"),
            QByteArray(R"({"version": 2, "recipes": []})"),
            recipeFile(R"({"deck": "left", "param": "eq_low", "points": [{"bar": 0, "value": 1}]})"),
            recipeFile(R"({"deck": "incoming", "param": "eq_bass", "points": [{"bar": 0, "value": 1}]})"),
            recipeFile(R"({"deck": "incoming", "param": "eq_low", "points": [
                {"bar": 0, "value": 1}, {"bar": 1, "value": 0, "shape": "wobble"}]})"),
            recipeFile(R"({"deck": "incoming", "param": "eq_low", "points": [
                {"bar": 1, "value": 1}, {"bar": 0.5, "value": 0}]})"),
            recipeFile(R"({"deck": "incoming", "param": "eq_low", "points": [
                {"bar": 3, "value": 1}]})"),
            recipeFile(R"({"deck": "incoming", "param": "eq_low", "points": [
                {"bar": 0, "value": 5}]})"),
            recipeFile(R"({"deck": "incoming", "param": "filter", "points": [
                {"bar": 0, "value": 1.5}]})"),
            recipeFile(R"({"deck": "incoming", "param": "eq_low", "points": [
                {"bar": 0, "value": "loud"}]})"),
            recipeFile(R"({"deck": "incoming", "param": "eq_low", "points": []})"),
            recipeFile(R"({"deck": "incoming", "param": "eq_low", "points": [{"bar": 0, "value": 1}]},
                          {"deck": "incoming", "param": "eq_low", "points": [{"bar": 0, "value": 0}]})"),
            recipeFile(R"({"deck": "incoming", "param": "eq_low", "points": [{"bar": 0, "value": 1}]})",
                    R"("prepare_incoming": {"eq_low": "current"},)"),
            recipeFile(R"({"deck": "incoming", "param": "eq_low", "points": [{"bar": 0, "value": 1}]})",
                    R"("incoming_play_at_bar": 5,)"),
            QByteArray(R"({"version": 1, "recipes": [{"id": "a", "length_bars": 1, "lanes": []},
                {"id": "a", "length_bars": 2, "lanes": []}]})"),
            QByteArray(R"({"version": 1, "recipes": [{"id": "a", "length_bars": 0, "lanes": []}]})"),
            QByteArray(R"({"version": 1, "settings": {"arm_quantum_bars": 0}, "recipes": []})"),
    };
    for (const QByteArray& json : broken) {
        AutomixRecipeBook book = AutomixRecipeBook::builtin();
        QString error;
        EXPECT_FALSE(AutomixRecipeBook::parse(json, &book, &error)) << json.toStdString();
        EXPECT_FALSE(error.isEmpty()) << json.toStdString();
        EXPECT_EQ(5, book.size()) << json.toStdString();
    }
}

TEST_F(AutomixRecipeTest, SelectorFallsBackToBuiltinRecipes) {
    AutomixRecipeBook book;
    QString error;
    ASSERT_TRUE(AutomixRecipeBook::parse(
            QByteArray(R"({"version": 1, "recipes": [
                {"id": "standard8", "name": "Mine", "length_bars": 6, "lanes": []}]})"),
            &book,
            &error))
            << error.toStdString();
    // The user's own standard8 wins.
    EXPECT_EQ(QStringLiteral("Mine"), book.forSelectorIndex(2)->name);
    // Slots missing from the file use the built-in recipe.
    ASSERT_NE(nullptr, book.forSelectorIndex(3));
    EXPECT_EQ(QStringLiteral("lung16"), book.forSelectorIndex(3)->id);
    // Out of range -> default slot.
    EXPECT_EQ(QStringLiteral("standard8"), book.forSelectorIndex(42)->id);
}

TEST_F(AutomixRecipeTest, StemLanesAndVocalGuardParse) {
    AutomixRecipeBook book;
    QString error;
    ASSERT_TRUE(AutomixRecipeBook::parse(recipeFile(R"(
        {"deck": "outgoing", "param": "stem_vocals", "points": [
            {"bar": 0, "value": "current"}, {"bar": 1, "value": "kill", "shape": "cos"}]},
        {"deck": "incoming", "param": "stem_drums", "points": [
            {"bar": 0, "value": 0.5}]})",
                                                    R"("vocal_guard": {"max_wait_bars": 4, "fade_beats": 1},)"),
            &book,
            &error))
            << error.toStdString();
    const AutomixRecipe& r = *book.find(QStringLiteral("t"));
    ASSERT_EQ(2u, r.lanes.size());
    EXPECT_EQ(AutomixParam::StemVocals, r.lanes[0].param);
    EXPECT_EQ(AutomixParam::StemDrums, r.lanes[1].param);
    EXPECT_TRUE(r.vocalGuard.enabled);
    EXPECT_DOUBLE_EQ(16.0, r.vocalGuard.maxWaitBeats);
    EXPECT_DOUBLE_EQ(1.0, r.vocalGuard.fadeBeats);
}

TEST_F(AutomixRecipeTest, VocalGuardDefaultsOnAndCanBeSwitchedOff) {
    const AutomixRecipe& standard = *AutomixRecipeBook::builtin().find(QStringLiteral("standard8"));
    EXPECT_TRUE(standard.vocalGuard.enabled);
    EXPECT_DOUBLE_EQ(32.0, standard.vocalGuard.maxWaitBeats);

    AutomixRecipeBook book;
    QString error;
    ASSERT_TRUE(AutomixRecipeBook::parse(
            recipeFile("", R"("vocal_guard": {"mode": "off"},)"), &book, &error))
            << error.toStdString();
    EXPECT_FALSE(book.find(QStringLiteral("t"))->vocalGuard.enabled);

    for (const char* bad : {R"("vocal_guard": {"mode": "maybe"},)",
                 R"("vocal_guard": {"max_wait_bars": 0},)",
                 R"("vocal_guard": 3,)"}) {
        AutomixRecipeBook untouched = AutomixRecipeBook::builtin();
        EXPECT_FALSE(AutomixRecipeBook::parse(recipeFile("", bad), &untouched, &error)) << bad;
    }
}

TEST_F(AutomixRecipeTest, KeyGuardDefaultsOnAndCanBeSwitchedOff) {
    const AutomixRecipe& standard = *AutomixRecipeBook::builtin().find(QStringLiteral("standard8"));
    EXPECT_TRUE(standard.keyGuard.enabled);
    EXPECT_DOUBLE_EQ(2.0, standard.keyGuard.fadeBeats);

    AutomixRecipeBook book;
    QString error;
    ASSERT_TRUE(AutomixRecipeBook::parse(
            recipeFile("", R"("key_guard": {"mode": "off"},)"), &book, &error))
            << error.toStdString();
    EXPECT_FALSE(book.find(QStringLiteral("t"))->keyGuard.enabled);
    ASSERT_TRUE(AutomixRecipeBook::parse(
            recipeFile("", R"("key_guard": {"fade_beats": 4},)"), &book, &error))
            << error.toStdString();
    EXPECT_TRUE(book.find(QStringLiteral("t"))->keyGuard.enabled);
    EXPECT_DOUBLE_EQ(4.0, book.find(QStringLiteral("t"))->keyGuard.fadeBeats);

    for (const char* bad : {R"("key_guard": {"mode": "maybe"},)",
                 R"("key_guard": {"fade_beats": -1},)",
                 R"("key_guard": "on",)"}) {
        AutomixRecipeBook untouched = AutomixRecipeBook::builtin();
        EXPECT_FALSE(AutomixRecipeBook::parse(recipeFile("", bad), &untouched, &error)) << bad;
    }
}

TEST_F(AutomixRecipeTest, StemValuesAreCappedAtUnity) {
    AutomixRecipeBook book;
    QString error;
    EXPECT_FALSE(AutomixRecipeBook::parse(recipeFile(R"(
        {"deck": "incoming", "param": "stem_vocals", "points": [{"bar": 0, "value": 2}]})"),
            &book,
            &error));
}
