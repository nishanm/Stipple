# name: Warp Tunnel
# summary: Rectangular rings rush towards you through a colour-shifting tunnel.
# author: Stipple
# tags: animation, 3d, hypnotic
# panel: 52x16

import math

class App
  def draw()
	clear(rgb(0, 0, 0))
	var ph = elapsed_ms() / 350.0
	var cx = width() / 2
	var cy = height() / 2
	var wob = math.sin(ph * 0.7) * 3.0

	for k : 0 .. 9
	  var f = ((k + ph) % 10.0) / 10.0
	  var e = f * f
	  var hw = 1 + int(e * 34)
	  var hh = 1 + int(e * 17)
	  var px = cx + int(wob * (1.0 - f))
	  var v = f
	  var r = int(v * (128 + 127 * math.sin(ph + f * 3.0)))
	  var g = int(v * (128 + 127 * math.sin(ph + f * 3.0 + 2.094)))
	  var b = int(v * (128 + 127 * math.sin(ph + f * 3.0 + 4.188)))
	  rect(px - hw, cy - hh, hw * 2, hh * 2, rgb(r, g, b))
	end
  end
end

return App()
