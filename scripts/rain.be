# name: Rain
# summary: Slanted rain falling past, with the odd distant flash of lightning.
# author: Stipple
# tags: ambient, animation, weather, relaxing
# panel: 52x16

import math

class App
  var dx, dy, dspeed   # drop position and fall speed, parallel arrays
  var flash            # frames of lightning remaining, 0 when clear

  def init()
	self.dx = []
	self.dy = []
	self.dspeed = []
	self.flash = 0
	# Thirty drops is a steady drizzle across 52 pixels; push this much higher
	# and the individual streaks stop being legible.
	for i : 0 .. 29
	  self.dx.push(math.rand() % width())
	  self.dy.push(math.rand() % height())
	  self.dspeed.push(1 + (i % 3))
	end
  end

  def draw()
	# Lightning lifts the whole background for a couple of frames, then decays.
	if self.flash > 0
	  var g = self.flash * 30
	  clear(rgb(g, g, g + 10))
	  self.flash -= 1
	else
	  clear(rgb(0, 0, 6))
	  # Roughly one frame in ninety opens a new flash. Rare on purpose.
	  if (math.rand() % 90) == 0
		self.flash = 3
	  end
	end

	for i : 0 .. size(self.dx) - 1
	  var speed = self.dspeed[i]
	  var x = self.dx[i]
	  var y = self.dy[i]

	  # Faster drops are nearer, so they are brighter and given a short tail;
	  # the slow ones stay as single dim dots in the background.
	  var level = 60 + speed * 55
	  pixel(x, y, rgb(level / 3, level / 2, level))
	  if speed >= 3 && y - 1 >= 0
		pixel(x, y - 1, rgb(level / 6, level / 4, level / 2))
	  end

	  # Fall and drift one pixel sideways so the rain slants rather than
	  # dropping dead straight, which always looks like a bug.
	  y += speed
	  x += 1
	  if y >= height()
		y = 0
		x = math.rand() % width()
	  end
	  if x >= width()
		x = 0
	  end
	  self.dx[i] = x
	  self.dy[i] = y
	end
  end
end

return App()
