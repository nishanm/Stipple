# name: DNA Helix
# summary: A double helix twists across the panel, its strands passing in front of and behind each other.
# author: Stipple
# tags: animation, science, hypnotic
# panel: 52x16

import math

class App
  def draw()
	clear(rgb(0, 2, 10))
	var ph = now_ms() / 300.0
	for x : 0 .. width() - 1
	  var a = x * 0.32 + ph
	  var s = math.sin(a)
	  var c = math.cos(a)
	  var y1 = 8 + int(s * 5)
	  var y2 = 8 - int(s * 5)

	  # Base pairs every third column, dim, drawn first so strands cover them.
	  if x % 3 == 0
		line(x, y1, x, y2, rgb(40, 60, 90))
	  end

	  # cos() says which strand is nearer - the nearer one is brighter and thicker.
	  var near = 130 + int(c * 120)
	  var far = 130 - int(c * 120)
	  pixel(x, y1, rgb(near, 60, 60))
	  pixel(x, y2, rgb(60, 90, far))
	  if c > 0.3
		pixel(x, y1 + 1, rgb(near / 2, 20, 20))
	  elif c < -0.3
		pixel(x, y2 + 1, rgb(20, 30, far / 2))
	  end
	end
  end
end

return App()
