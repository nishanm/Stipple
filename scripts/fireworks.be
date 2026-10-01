# name: Fireworks
# summary: Rockets climb, burst into coloured sparks, then fade and fall. One at a time.
# author: Stipple
# tags: ambient, animation, celebration
# panel: 52x16

import math

class App
  var phase          # "climb" or "burst"
  var rx, ry, ry_top # rocket position and the height it detonates at
  var hue            # colour of the current burst, kept across the climb
  var sx, sy, svx, svy, life  # spark arrays, parallel

  def init()
	self.sx = []
	self.sy = []
	self.svx = []
	self.svy = []
	self.life = []
	self.launch()
  end

  # Pick a launch column and a detonation height, and choose the burst colour
  # up front so the rocket's trail hints at what is coming.
  def launch()
	self.phase = "climb"
	self.rx = 6 + (math.rand() % (width() - 12))
	self.ry = height() - 1
	self.ry_top = 2 + (math.rand() % 6)
	self.hue = math.rand() % 6
  end

  # Map a small hue index to a bright, saturated rgb. Kept deliberately few
  # so bursts stay recognisably one colour rather than a muddy gradient.
  def colour(h, level)
	if   h == 0 return rgb(level, level / 4, level / 4)      # red
	elif h == 1 return rgb(level, level / 2, 20)            # amber
	elif h == 2 return rgb(30, level, 40)                  # green
	elif h == 3 return rgb(40, level / 2, level)           # blue
	elif h == 4 return rgb(level, 40, level)               # magenta
	else        return rgb(level, level, level / 2)        # gold
	end
  end

  # Seed sparks flying out from the detonation point. Velocities are integers
  # scaled by four (see draw) so a slow-moving panel still gives smooth arcs.
  def burst()
	self.phase = "burst"
	for i : 0 .. 15
	  var a = (i * 4) % 32
	  self.sx.push(self.rx * 4)
	  self.sy.push(self.ry * 4)
	  self.svx.push((math.rand() % 9) - 4)
	  self.svy.push((math.rand() % 9) - 6)
	  self.life.push(14 + (math.rand() % 8))
	end
  end

  def draw()
	clear(rgb(0, 0, 0))

	if self.phase == "climb"
	  # A short fading tail sells the climb better than a single lit pixel.
	  pixel(self.rx, self.ry, rgb(180, 140, 60))
	  if self.ry + 1 < height()
		pixel(self.rx, self.ry + 1, rgb(60, 45, 20))
	  end
	  self.ry -= 1
	  if self.ry <= self.ry_top
		self.burst()
	  end
	  return
	end

	# Burst: advance every spark, apply a little gravity, dim as it ages.
	var living = 0
	for i : 0 .. size(self.sx) - 1
	  if self.life[i] <= 0
		continue
	  end
	  living += 1

	  self.sx[i] += self.svx[i]
	  self.sy[i] += self.svy[i]
	  self.svy[i] += 1          # gravity pulls sparks back down
	  self.life[i] -= 1

	  var px = self.sx[i] / 4
	  var py = self.sy[i] / 4
	  if px >= 0 && px < width() && py >= 0 && py < height()
		var level = 40 + self.life[i] * 12
		if level > 255 level = 255 end
		pixel(px, py, self.colour(self.hue, level))
	  end
	end

	# Once the sky is empty, clear the arrays and send up the next rocket.
	if living == 0
	  self.sx = []
	  self.sy = []
	  self.svx = []
	  self.svy = []
	  self.life = []
	  self.launch()
	end
  end
end

return App()
