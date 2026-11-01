#include "config/Layout.h"

namespace kickemu
{
    namespace
    {
        float f (const juce::var& v, float fallback = 0.0f) { return v.isVoid() ? fallback : (float) (double) v; }

        juce::Point<float> point (const juce::var& v)
        {
            if (auto* a = v.getArray(); a != nullptr && a->size() >= 2)
                return { f ((*a)[0]), f ((*a)[1]) };
            return {};
        }

        juce::Rectangle<float> rectangle (const juce::var& v)
        {
            if (auto* a = v.getArray(); a != nullptr && a->size() >= 4)
                return { f ((*a)[0]), f ((*a)[1]), f ((*a)[2]), f ((*a)[3]) };
            return {};
        }

        int i (const juce::var& v, int fallback = -1) { return v.isVoid() ? fallback : (int) v; }

        juce::Colour colour (const juce::var& v)
        {
            auto s = v.toString().trimCharactersAtStart ("#");
            if (s.isEmpty())
                return {};
            return juce::Colour ((juce::uint32) (0xff000000u | (juce::uint32) s.getHexValue32()));
        }

        LayoutText text (const juce::var& v)
        {
            LayoutText t;
            t.text = v["text"].toString();
            t.position = { f (v["x"]), f (v["y"]) };
            t.size = f (v["size"], 3.0f);
            if (v.hasProperty ("colour")) t.colour = v["colour"].toString();
            if (v.hasProperty ("align")) t.align = v["align"].toString();
            if (v.hasProperty ("font")) t.font = v["font"].toString();
            return t;
        }

        LayoutStrip strip (const juce::var& v)
        {
            LayoutStrip s;
            s.width = f (v["width"]);
            s.height = f (v["height"]);
            s.panel = rectangle (v["panel"]);
            s.footHeight = f (v["foot_height"]);
            if (auto* a = v["feet"].getArray())
                for (auto& foot : *a)
                    s.feet.add (point (foot));
            if (auto* a = v["dividers"].getArray())
                for (auto& d : *a)
                    s.dividers.add (f (d));
            s.dividerTop = f (v["divider_top"]);
            s.dividerBottom = f (v["divider_bottom"]);
            if (auto* a = v["screws"].getArray())
                for (auto& sc : *a)
                    s.screws.add (point (sc));
            s.screwRadius = f (v["screw_radius"]);
            if (auto* a = v["connectors"].getArray())
            {
                for (auto& c : *a)
                {
                    LayoutConnector con;
                    con.id = c["id"].toString();
                    con.type = c["type"].toString();
                    con.centre = point (c["centre"]);
                    con.rect = rectangle (c["rect"]);
                    con.size = point (c["size"]);
                    con.radius = f (c["radius"]);
                    con.channel = i (c["channel"]);
                    s.connectors.add (con);
                }
            }
            if (auto* a = v["texts"].getArray())
                for (auto& t : *a)
                    s.texts.add (text (t));
            if (v.hasProperty ("wordmark"))
            {
                s.wordmark = text (v["wordmark"]);
                s.wordmark.text = "behringer";
                s.wordmark.font = "wordmark";
            }
            return s;
        }
    }

    juce::Result Layout::parse (const juce::String& json)
    {
        juce::var root;
        if (auto r = juce::JSON::parse (json, root); r.failed())
            return juce::Result::fail ("layout.json: " + r.getErrorMessage());

        auto top = root["top"];
        if (! top.isObject())
            return juce::Result::fail ("layout.json: missing top view");

        width = f (top["width"], width);
        height = f (top["height"], height);
        cornerRadius = f (top["corner_radius"], cornerRadius);
        bevel = f (top["bevel"], bevel);

        controls.clear();
        if (auto* a = top["controls"].getArray())
        {
            for (auto& c : *a)
            {
                LayoutControl lc;
                lc.id = c["id"].toString();
                lc.type = c["type"].toString();
                lc.rect = rectangle (c["rect"]);
                lc.centre = point (c["centre"]);
                lc.radius = f (c["radius"]);
                lc.label = c["label"].toString();
                lc.icon = c["icon"].toString();
                lc.key = c["key"].toString();
                lc.tip = c["tip"].toString();
                lc.colour = colour (c["colour"]);
                lc.index = i (c["index"]);
                lc.hw = i (c["hw"]);
                lc.shiftHw = i (c["shift_hw"]);
                lc.led = i (c["led"]);
                lc.knob = i (c["knob"]);
                lc.pushHw = i (c["push_hw"]);
                auto q = point (c["quadrant"]);
                lc.quadrant = { (int) q.x, (int) q.y };
                lc.extra = c;
                controls.push_back (lc);
            }
        }

        texts.clear();
        if (auto* a = top["texts"].getArray())
            for (auto& t : *a)
                texts.push_back (text (t));

        decor.clear();
        if (auto* a = top["decor"].getArray())
        {
            for (auto& d : *a)
            {
                LayoutDecor ld;
                ld.type = d["type"].toString();
                ld.rect = rectangle (d["rect"]);
                ld.a = d.hasProperty ("centre") ? point (d["centre"]) : point (d["from"]);
                ld.b = point (d["to"]);
                ld.radius = f (d["radius"]);
                ld.extra = d;
                decor.push_back (ld);
            }
        }

        rear = strip (root["rear"]);
        front = strip (root["front"]);

        if (controls.empty())
            return juce::Result::fail ("layout.json: no controls");
        return juce::Result::ok();
    }

    const LayoutControl* Layout::find (const juce::String& id) const
    {
        for (auto& c : controls)
            if (c.id == id)
                return &c;
        return nullptr;
    }

    std::vector<const LayoutControl*> Layout::ofType (const juce::String& type) const
    {
        std::vector<const LayoutControl*> out;
        for (auto& c : controls)
            if (c.type == type)
                out.push_back (&c);
        return out;
    }
}
