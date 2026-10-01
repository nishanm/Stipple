# name: Thunderstorm
# summary: Rain slants across a black sky, and now and then a jagged bolt splits it and lights the whole panel.
# author: Stipple
# tags: ambient, weather, dramatic
# panel: 52x16

import math

class App
  var dx
  var dy
  var bolt
  var flash
  var last

  def init()
	self.dx = []
	self.dy = []
	self.bolt = []
	self.flash = 0
	self.last = now_ms()
	for i : 0 .. 39
	  self.dx.push(math.rand() % 60 * 1.0)
	  self.dy.push(math.rand() % 16 * 1.0)
	end
  end

  def strike()
	self.bolt = []
	var x = 10 + math.rand() % 32
	for y : 0 .. 15
	  self.bolt.push(x)
	  x += (math.rand() % 3) - 1
	end
	self.flash = 7
  end

  def step()
	for i : 0 .. self.dx.size() - 1
	  self.dy[i] += 1.6
	  self.dx[i] -= 0.7
	  if self.dy[i] > 16 || self.dx[i] < -2
		self.dy[i] = 0.0
		self.dx[i] = 8.0 + math.rand() % 60
	  end
	end
	if self.flash > 0
	  self.flash -= 1
	elif (math.rand() % 45) == 0
	  self.strike()
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 40
	  self.last = t
	  self.step()
	end
	var f = self.flash * self.flash
	clear(rgb(2 + f, 3 + f, 8 + f * 2))
	for i : 0 .. self.dx.size() - 1
	  var x = int(self.dx[i])
	  var y = int(self.dy[i])
	  pixel(x, y, rgb(70, 90, 150))
	  pixel(x + 1, y - 1, rgb(30, 40, 80))
	end
	if self.flash > 3
	  for y : 0 .. self.bolt.size() - 1
		pixel(self.bolt[y], y, rgb(255, 255, 255))
		pixel(self.bolt[y] + 1, y, rgb(160, 170, 255))
	  end
	end
  end
end

return App()
