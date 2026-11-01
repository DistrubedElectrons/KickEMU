#include "panel/PanelControl.h"

namespace kickemu
{
    PanelContext::PanelContext (const Theme& t, PanelState& s, McuProtocol& m, const Config& c)
        : theme (t), state (s), mcu (m), config (c)
    {
        blinkPeriodSeconds = c.getInt ("host.ui.blink_period_ms", 500) / 1000.0;
        fadeMs = (float) c.getDouble ("host.ui.led_fade_ms", 70.0);
        shiftAlternates = c.getBool ("host.mcu.shift_alternates", true);
        buttonLedFromPalette = c.getString ("host.mcu.button_led_colour", "fixed") == "palette";
        jogRingDefault = c.getString ("host.mcu.jog_ring_default", "connected");
        velocityMin = c.getInt ("host.mcu.pad_velocity_min", 24);
        velocityMax = c.getInt ("host.mcu.pad_velocity_max", 127);
        aftertouchIntervalMs = c.getInt ("host.mcu.aftertouch_interval_ms", 30);
        longPressMs = c.getInt ("host.mcu.long_press_ms", 0);
        encoderPxPerStep = (float) c.getDouble ("host.mcu.encoder_drag_px_per_step", 6.0);
        jogDegreesPerStep = (float) c.getDouble ("host.mcu.jog_drag_degrees_per_step", 15.0);
        wheelStepsPerNotch = c.getInt ("host.mcu.wheel_steps_per_notch", 1);
        noise = paint::noiseTile (128, 0x4b1c);
    }

    PanelControl::PanelControl (PanelContext& context, const LayoutControl& definition, float marginMillimetres)
        : ctx (context), def (definition), marginMm (marginMillimetres)
    {
        setRepaintsOnMouseActivity (false);
        setPaintingIsUnclipped (false);
        auto tip = def.tip.isNotEmpty() ? def.tip : def.id;
        if (auto k = keyHint(); k.isNotEmpty())
            tip << "  [" << k << "]";
        setTooltip (tip);
    }

    juce::String PanelControl::keyHint() const
    {
        if (def.key.isEmpty())
            return {};
        auto k = def.key;
        if (k == "\\") return "\\";
        return k.length() == 1 ? k.toUpperCase() : k.substring (0, 1) + k.substring (1).toLowerCase();
    }

    void PanelControl::place (float scale, float physicalScale)
    {
        pxPerMm = scale;
        pixelScale = physicalScale;
        margin = marginMm * scale;
        auto b = def.getBounds() * scale;
        auto outer = b.expanded (margin).getSmallestIntegerContainer();
        setBounds (outer);
        body = b.translated ((float) -outer.getX(), (float) -outer.getY());
        rebuild();
        repaint();
    }

    float PanelControl::approach (float current, float target, float dtMs, float tauMs)
    {
        if (tauMs <= 0.0f)
            return target;
        const float k = 1.0f - std::exp (-dtMs / tauMs);
        const float next = current + (target - current) * k;
        return std::abs (next - target) < 0.002f ? target : next;
    }
}
