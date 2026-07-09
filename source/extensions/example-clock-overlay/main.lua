-- Example TapeXPlayer extension (type = lua).
--
-- Demonstrates the `fstp` host API. A human or an AI can author an extension by
-- editing this file and the manifest next to it. See FSTPLuaExtension.h for the
-- full API. This is a read-only overlay; it never changes playback.
--
-- The same API also lets an extension DRIVE the player and export findings
-- (fstp.seek/play/pause/set_speed/step, fstp.screenshot, fstp.add_marker,
-- fstp.export_csv) — the foundation for scientific automation or an external
-- MCP/AI bridge.

fstp.log("clock-overlay: loaded")

fstp.on_load(function(path)
    fstp.log("clock-overlay: file loaded -> " .. path)
end)

fstp.on_render(function()
    local s = fstp.state()
    if not s.active then return end
    -- Top-left, clear of the OSD timecode/meters.
    fstp.draw_text(20, 20, "LUA  " .. s.timecode, 20, 120, 200, 255)
    fstp.draw_text(20, 48,
        string.format("frame %d  @ %.2f fps  %s", s.frame, s.fps, s.playing and "play" or "pause"),
        16, 150, 150, 160)
end)
