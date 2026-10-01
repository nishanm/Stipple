# name: Sky Clock
# summary: The sun rises, crosses and sets across the panel in step with the real time; at night, a moon and stars.
# author: Stipple
# tags: clock, ambient, time, sky
# panel: 52x16

import math
import string

class App
  def mix(a, b, f)
	return int(a + (b - a) * f)
  end

  def draw()
	if !time_known()
	  clear(rgb(0, 0, 0))
	  text(8, 5, "no time", rgb(180, 60, 60))
	  return
	end

	var h = hour() + minute() / 60.0
	# Sun is up from 06:00 to 20:00.
	var sunUp = h >= 6.0 && h <= 20.0
	var p = (h - 6.0) / 14.0
	var elev = sunUp ? math.sin(p * math.pi) : 0.0

	if sunUp
	  # Deep blue at noon, orange at the horizon.
	  var f = elev
	  if f > 1.0
		f = 1.0
	  end
	  var warm = f < 0.3 ? (0.3 - f) / 0.3 : 0.0
	  clear(rgb(self.mix(40, 80, f) + int(warm * 190), self.mix(60, 170, f) + int(warm * 70), self.mix(100, 240, f) - int(warm * 60)))
	  var sx = 26 - int(math.cos(p * math.pi) * 24)
	  var sy = 13 - int(elev * 12)
	  rect_fill(sx - 1, sy - 1, 3, 3, rgb(255, 230, 90))
	  pixel(sx - 2, sy, rgb(255, 200, 60))
	  pixel(sx + 2, sy, rgb(255, 200, 60))
	  pixel(sx, sy - 2, rgb(255, 200, 60))
	  pixel(sx, sy + 2, rgb(255, 200, 60))
	else
	  clear(rgb(2, 3, 16))
	  for s : 0 .. 15
		var tw = (elapsed_ms() / 400 + s * 3) % 5
		var b = tw == 0 ? 60 : 130
		pixel((s * 19 + 3) % 52, (s * 7) % 10, rgb(b, b, b + 40))
	  end
	  # Moon.
	  rect_fill(40, 2, 4, 4, rgb(230, 230, 210))
	  rect_fill(42, 1, 3, 3, rgb(2, 3, 16))
	end

	# Ground, and the time in the corner.
	rect_fill(0, 14, 52, 2, sunUp ? rgb(20, 70, 30) : rgb(4, 14, 8))
	text(1, 0, string.format("%02d:%02d", hour(), minute()), sunUp ? rgb(20, 30, 60) : rgb(120, 120, 160))
  end
end

return App()
