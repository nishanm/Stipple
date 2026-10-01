# name: Aurora
# summary: Northern lights - green and violet curtains that drift, brighten and fade.
# author: Stipple
# tags: ambient, animation, relaxing, colour
# panel: 52x16

import math

class App
  var t

  def init()
	self.t = 0.0
  end

  def draw()
	self.t += 0.05
	var t = self.t
	clear(rgb(0, 0, 8))

	for x : 0 .. width() - 1
	  # Two waves per column set the height of the curtain's bright edge; the
	  # slower one drifts the whole sheet, the faster adds the shimmer.
	  var wave = math.sin(x / 7.0 + t) * 3.5
	  wave += math.sin(x / 3.0 - t * 1.7) * 1.5
	  var edge = int(height() / 2 + wave)

	  for y : 0 .. height() - 1
		# Distance below the bright edge; light fades as it falls away.
		var d = y - edge
		if d < 0 d = -d end
		var level = 200 - d * 45
		if level <= 0
		  continue
		end

		# Colour shifts along the panel: green over most of it, tipping into
		# violet at the right-hand end the way real aurora does.
		var mix = int(x * 255 / width())
		var g = level
		var r = (level * mix) / 400
		var b = (level * mix) / 300
		pixel(x, y, rgb(r, g, b))
	  end
	end
  end
end

return App()
