# name: Kaleidoscope
# summary: A rippling interference pattern mirrored four ways, so it always blooms out of the centre.
# author: Stipple
# tags: animation, generative, hypnotic, colourful
# panel: 52x16

import math

class App
  def draw()
	var t = elapsed_ms() / 600.0
	clear(rgb(0, 0, 0))
	# Compute one quadrant; mirror it to the other three.
	for y : 0 .. 7
	  for x : 0 .. 25
		var dx = 25 - x
		var dy = (7 - y) * 2
		var d = math.sqrt(dx * dx + dy * dy)
		var v = math.sin(d * 0.55 - t * 2.0) + math.sin(dx * 0.3 + t) * math.sin(dy * 0.35 - t * 0.8)
		var r = 128 + int(127 * math.sin(v * 1.6 + t))
		var g = 128 + int(127 * math.sin(v * 1.6 + 2.094))
		var b = 128 + int(127 * math.sin(v * 1.6 + 4.188 - t))
		var c = rgb(r, g, b)
		pixel(x, y, c)
		pixel(51 - x, y, c)
		pixel(x, 15 - y, c)
		pixel(51 - x, 15 - y, c)
	  end
	end
  end
end

return App()
