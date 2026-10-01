# name: Radar
# summary: A sweeping radar arm with fading trails and blips that glow when it passes over them.
# author: Stipple
# tags: animation, ambient, retro
# panel: 52x16

import math

class App
  var angle
  var last
  var bx
  var by
  var seen

  def init()
	self.angle = 0.0
	self.last = now_ms()
	self.bx = []
	self.by = []
	self.seen = []
	self.scatter()
  end

  def scatter()
	self.bx = []
	self.by = []
	self.seen = []
	for i : 0 .. 4
	  self.bx.push(math.rand() % 60 - 30)
	  self.by.push(math.rand() % 16 - 8)
	  self.seen.push(0)
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 33
	  self.last = t
	  self.angle = self.angle + 0.09
	  if self.angle > 6.2832
		self.angle = self.angle - 6.2832
		self.scatter()
	  end
	  for i : 0 .. self.seen.size() - 1
		if self.seen[i] > 0
		  self.seen[i] = self.seen[i] - 4
		end
		var a = math.atan2(self.by[i] * 1.0, self.bx[i] * 1.0)
		if a < 0
		  a = a + 6.2832
		end
		var d = a - self.angle
		if d > -0.12 && d < 0.12
		  self.seen[i] = 255
		end
	  end
	end

	clear(rgb(0, 6, 0))

	var cx = width() / 2
	var cy = height() / 2

	# Range rings, squashed to fit the panel.
	for r : [6, 12, 24]
	  for s : 0 .. 47
		var a = s * 0.1309
		pixel(cx + int(math.cos(a) * r), cy + int(math.sin(a) * r / 2.0), rgb(0, 30, 8))
	  end
	end

	# Sweep with a fading tail.
	for k : 0 .. 5
	  var a = self.angle - k * 0.07
	  var g = 255 - k * 45
	  line(cx, cy, cx + int(math.cos(a) * 30), cy + int(math.sin(a) * 15), rgb(0, g, g / 6))
	end

	for i : 0 .. self.seen.size() - 1
	  if self.seen[i] > 0
		var v = self.seen[i]
		rect_fill(cx + self.bx[i], cy + self.by[i], 2, 2, rgb(v, 255, v / 2))
	  end
	end

	pixel(cx, cy, rgb(255, 255, 255))
  end
end

return App()
