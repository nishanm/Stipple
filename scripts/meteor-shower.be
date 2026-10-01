# name: Meteor Shower
# summary: Bright meteors streak across with glowing tails that fade behind them.
# author: Stipple
# tags: animation, space, ambient
# panel: 52x16

import math

class App
  var buf              # persistent brightness field, one entry per pixel
  var mx, my, mvx, mvy, mhue  # meteors, parallel arrays (positions x16)

  def init()
	self.buf = []
	for i : 0 .. width() * height() - 1
	  self.buf.push(0)
	end
	self.mx = []
	self.my = []
	self.mvx = []
	self.mvy = []
	self.mhue = []
	for i : 0 .. 3
	  self.spawn(i)
	end
  end

  # Launch a meteor from off the top-left, angled down and to the right.
  def spawn(i)
	var startx = -(math.rand() % 40)
	var starty = math.rand() % height()
	var vx = 3 + (math.rand() % 3)
	var vy = 1 + (math.rand() % 2)
	var hue = math.rand() % 4
	if size(self.mx) <= i
	  self.mx.push(startx * 16)
	  self.my.push(starty * 16)
	  self.mvx.push(vx)
	  self.mvy.push(vy)
	  self.mhue.push(hue)
	else
	  self.mx[i] = startx * 16
	  self.my[i] = starty * 16
	  self.mvx[i] = vx
	  self.mvy[i] = vy
	  self.mhue[i] = hue
	end
  end

  def tint(hue, level)
	if   hue == 0 return rgb(level, level, level)              # white
	elif hue == 1 return rgb(level, (level * 3) / 4, level / 3) # warm gold
	elif hue == 2 return rgb(level / 3, (level * 3) / 4, level) # icy blue
	else          return rgb(level, level / 3, (level * 3) / 4) # rose
	end
  end

  def draw()
	# Fade the whole field a little; this is what leaves the tails behind.
	for i : 0 .. size(self.buf) - 1
	  var v = self.buf[i] - 28
	  if v < 0 v = 0 end
	  self.buf[i] = v
	end

	# Advance each meteor and stamp its head into the field at full brightness.
	for m : 0 .. size(self.mx) - 1
	  self.mx[m] += self.mvx[m] * 16 / 4
	  self.my[m] += self.mvy[m] * 16 / 4
	  var px = self.mx[m] / 16
	  var py = self.my[m] / 16

	  if px >= 0 && px < width() && py >= 0 && py < height()
		self.buf[py * width() + px] = 255
	  end

	  # Recycle once it has fully left the panel.
	  if px >= width() || py >= height()
		self.spawn(m)
	  end
	end

	# Render the field. Every meteor shares hue 0's white core here to keep
	# the draw cheap; the fading buffer supplies all the tail work.
	clear(rgb(0, 0, 4))
	for y : 0 .. height() - 1
	  for x : 0 .. width() - 1
		var level = self.buf[y * width() + x]
		if level > 0
		  pixel(x, y, self.tint(2, level))
		end
	  end
	end

	# A faint scatter of background stars so the meteors have a sky to cross.
	for s : 0 .. 5
	  var sx = (s * 9 + 3) % width()
	  var sy = (s * 5 + 2) % height()
	  if self.buf[sy * width() + sx] == 0
		pixel(sx, sy, rgb(30, 30, 40))
	  end
	end
  end
end

return App()
