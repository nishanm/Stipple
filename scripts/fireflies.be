# name: Fireflies
# summary: Warm lights wander a summer night, each one slowly glowing and fading on its own rhythm.
# author: Stipple
# tags: ambient, nature, relaxing
# panel: 52x16

import math

class App
  var x
  var y
  var a
  var ph
  var sp
  var last

  def init()
	self.x = []
	self.y = []
	self.a = []
	self.ph = []
	self.sp = []
	self.last = now_ms()
	for i : 0 .. 11
	  self.x.push(math.rand() % 52 * 1.0)
	  self.y.push(math.rand() % 16 * 1.0)
	  self.a.push((math.rand() % 628) / 100.0)
	  self.ph.push((math.rand() % 628) / 100.0)
	  self.sp.push(0.03 + (math.rand() % 10) / 150.0)
	end
  end

  def step()
	for i : 0 .. self.x.size() - 1
	  self.a[i] += ((math.rand() % 21) - 10) / 40.0
	  self.x[i] += math.cos(self.a[i]) * 0.35
	  self.y[i] += math.sin(self.a[i]) * 0.2
	  if self.x[i] < 0
		self.x[i] = 51.0
	  elif self.x[i] > 51
		self.x[i] = 0.0
	  end
	  if self.y[i] < 1
		self.a[i] = 1.2
	  elif self.y[i] > 14
		self.a[i] = -1.2
	  end
	  self.ph[i] += self.sp[i]
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 40
	  self.last = t
	  self.step()
	end
	clear(rgb(1, 5, 4))
	# Grass.
	for gx : 0 .. 25
	  var h = 1 + (gx * 7) % 3
	  line(gx * 2, 15, gx * 2, 15 - h, rgb(4, 24, 10))
	end
	for i : 0 .. self.x.size() - 1
	  var s = math.sin(self.ph[i])
	  var g = s > 0 ? int(s * s * 255) : 0
	  var x = int(self.x[i])
	  var y = int(self.y[i])
	  if g > 20
		pixel(x, y, rgb(g, g, g / 5))
		if g > 90
		  var h = g / 6
		  pixel(x - 1, y, rgb(h, h, 0))
		  pixel(x + 1, y, rgb(h, h, 0))
		  pixel(x, y - 1, rgb(h, h, 0))
		  pixel(x, y + 1, rgb(h, h, 0))
		end
	  else
		pixel(x, y, rgb(6, 8, 4))
	  end
	end
  end
end

return App()
