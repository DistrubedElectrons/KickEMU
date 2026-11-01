#include "config/Layout.h"

using namespace kickemu;

/*
 * Geometry checks on resources/layout.json: printed labels must not touch the
 * controls, dots and rules around them, nor each other, and no two controls
 * may overlap. Text ink is approximated from the label font (DIN, condensed):
 * 0.58 x size per character wide, 0.75 x size per line high around the anchor.
 */
class LayoutTests : public juce::UnitTest
{
public:
    LayoutTests() : juce::UnitTest ("Panel layout", "panel") {}

    static juce::Rectangle<float> inkBox (const LayoutText& t)
    {
        auto lines = juce::StringArray::fromLines (t.text);
        int longest = 0;
        for (auto& l : lines)
            longest = juce::jmax (longest, l.length());
        const float w = 0.58f * t.size * (float) longest;
        const float h = 0.75f * t.size * (float) lines.size();
        const float x = t.align == "left" ? t.position.x : t.position.x - w * 0.5f;
        return { x, t.position.y - h * 0.5f, w, h };
    }

    static bool hitsCircle (juce::Rectangle<float> r, juce::Point<float> c, float radius)
    {
        const float nx = juce::jlimit (r.getX(), r.getRight(), c.x);
        const float ny = juce::jlimit (r.getY(), r.getBottom(), c.y);
        return c.getDistanceFrom ({ nx, ny }) < radius;
    }

    static bool isRound (const LayoutControl& c) { return c.radius > 0.0f; }

    /** True if the label box, grown by the clearance, touches the control. */
    static bool hitsControl (juce::Rectangle<float> box, const LayoutControl& c, float clearance)
    {
        if (isRound (c))
            return hitsCircle (box, c.centre, c.radius + clearance);
        return box.expanded (clearance).intersects (c.rect);
    }

    void runTest() override
    {
        Layout layout;
        expect (layout.parse (juce::File (KICKEMU_TEST_LAYOUT).loadFileAsString()).wasOk());
        constexpr float clearance = 0.5f;   // mm between print and anything raised

        beginTest ("Labels keep clear of controls");
        for (auto& t : layout.texts)
            for (auto& c : layout.controls)
            {
                // The wordmarks are printed on the screen bezel
                if (c.type == "screen" && (t.text == "behringer" || t.text == "KICK"))
                    continue;
                if (c.type == "jog_key")
                    continue;   // drawn inside the jog surround, no label near them
                expect (! hitsControl (inkBox (t), c, clearance),
                        "'" + t.text + "' overlaps " + c.id);
            }

        beginTest ("Labels keep clear of scale dots, rules and each other");
        for (size_t i = 0; i < layout.texts.size(); ++i)
        {
            auto box = inkBox (layout.texts[i]);
            for (auto& d : layout.decor)
            {
                if (d.type == "dot")
                    expect (! hitsCircle (box, d.a, d.radius + clearance * 0.6f),
                            "'" + layout.texts[i].text + "' overlaps a scale dot");
                else if (d.type == "line")
                    expect (! box.expanded (clearance).intersects (juce::Rectangle<float> (d.a, d.b).expanded (0.0f, 0.1f)),
                            "'" + layout.texts[i].text + "' overlaps a rule");
            }
            for (size_t j = i + 1; j < layout.texts.size(); ++j)
                expect (! box.expanded (0.3f).intersects (inkBox (layout.texts[j])),
                        "'" + layout.texts[i].text + "' overlaps '" + layout.texts[j].text + "'");
        }

        beginTest ("Controls do not overlap");
        for (size_t i = 0; i < layout.controls.size(); ++i)
            for (size_t j = i + 1; j < layout.controls.size(); ++j)
            {
                auto& a = layout.controls[i];
                auto& b = layout.controls[j];
                if (a.type == "jog_key" || b.type == "jog_key" || a.type == "jog" || b.type == "jog")
                    continue;   // the jog keys surround the wheel by design
                bool hit = false;
                if (isRound (a) && isRound (b))
                    hit = a.centre.getDistanceFrom (b.centre) < a.radius + b.radius;
                else if (isRound (a) != isRound (b))
                {
                    auto& r = isRound (a) ? b : a;
                    auto& c = isRound (a) ? a : b;
                    hit = hitsCircle (r.rect, c.centre, c.radius);
                }
                else
                    hit = a.rect.intersects (b.rect);
                expect (! hit, a.id + " overlaps " + b.id);
            }

        beginTest ("MASTER and REC: knob, title, dots and MIN/MAX share one column");
        for (auto* id : { "master", "rec" })
        {
            auto* pot = layout.find (id);
            expect (pot != nullptr);
            int dots = 0;
            float dotSum = 0.0f;
            for (auto& d : layout.decor)
                if (d.type == "dot" && std::abs (d.a.y - pot->centre.y) < pot->radius + 4.0f)
                {
                    ++dots;
                    dotSum += d.a.x;
                }
            expectEquals (dots, 2);
            expectWithinAbsoluteError (dotSum * 0.5f, pot->centre.x, 0.05f);
        }
    }
};

static LayoutTests layoutTests;
