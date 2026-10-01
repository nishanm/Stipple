# name: Plasma
# summary: Classic rolling rainbow plasma, every pixel alive and shifting.
# author: Stipple
# tags: ambient, animation, generative, colour
# panel: 52x16

import math

class App
  var t

  def init()
	self.t = 0.0
  end

  # HSV-ish rainbow from a single angle (0..255). Cheap sixths of the wheel,
  # full saturation, so neighbouring pixels stay distinct colours.
  def wheel(pos)
	pos = pos % 256
	if pos < 85
	  return rgb(255 - pos * 3, pos * 3, 0)
	elif pos < 170
	  pos -= 85
	  return rgb(0, 255 - pos * 3, pos * 3)
	else
	  pos -= 170
	  return rgb(pos * 3, 0, 255 - pos * 3)
	end
  end

  def draw()
	self.t += 0.08
	var t = self.t

	for y : 0 .. height() - 1
	  for x : 0 .. width() - 1
		# Three overlapping waves at different angles and speeds. The diagonal
		# term (x+y) is what stops it looking like plain horizontal bars.
		var v = math.sin(x / 6.0 + t)
		v += math.sin(y / 4.0 - t)
		v += math.sin((x + y) / 7.0 + t * 0.7)
		v += math.sin(math.sqrt(real((x - 26) * (x - 26) + (y - 8) * (y - 8))) / 4.0 - t)

		# v is now in -4..4; fold it onto the colour wheel.
		var hue = int((v + 4.0) * 32.0)
		pixel(x, y, self.wheel(hue))
	  end
	end
  end
end

return App()
