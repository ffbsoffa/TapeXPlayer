-- Smart shuttle-locate: wind fast toward a target timecode with real transport
-- speed, decelerate as it nears, and land EXACTLY (final seek guarantees the frame).
-- Now that fast shuttle works, this is the closed-loop "locate" with tape feel.
local phase="settle"; local stable=0; local target=nil; local lastd=nil
fstp.on_load(function() phase="settle"; stable=0; target=nil end)
fstp.on_render(function()
  if phase=="done" then return end
  local s=fstp.state(); if not s.active or s.fps<=0 then return end
  if phase=="settle" then
    stable=stable+1
    if stable>120 then target=s.time+60.0; phase="wind"   -- demo: 60 s ahead
      fstp.log(string.format("locate -> %.1fs", target)) end
    return
  end
  local d=target - s.time; local ad=math.abs(d)
  local margin=math.max(0.5, math.abs(s.speed)*0.5)        -- brake zone scales with speed
  local crossed=(lastd~=nil) and ((d<=0)~=(lastd<=0)); lastd=d
  if ad<=margin or crossed then
    fstp.shuttle(0); fstp.seek(target); fstp.pause(); phase="done"
    fstp.log(string.format("locate: LANDED at %.2fs (approach err %.2fs, final seek exact)", target, d))
    return
  end
  local spd=math.min(16, math.max(2, ad*0.8)); if d<0 then spd=-spd end
  fstp.shuttle(spd)
  fstp.draw_text(20,80,string.format("LOCATE -> %.0fs | %+.0fs @ %.0fx", target, d, math.abs(spd)),18,240,180,80)
end)
