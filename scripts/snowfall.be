# name: Snowfall
# summary: Snow drifts down on a light wind and settles into drifts along the bottom, then melts away.
# author: Stipple
# tags: ambient, weather, relaxing, winter
# panel: 52x16

import math

class App
  var fx
  var fy
  var fs
  var pile
  var last
  var tick

  def init()
	self.fx = []
	self.fy = []
	self.fs = []
	self.pile = []
	self.tick = 0
	self.last = now_ms()
	for i : 0 .. 34
	  self.fx.push((math.rand() % 520) / 10.0)
	  self.fy.push((math.rand() % 150) / 10.0)
	  self.fs.push(0.12 + (math.rand() % 10) / 40.0)
	end
	for i : 0 .. width() - 1
	  self.pile.push(0)
	end
  end

  def step()
	self.tick += 1
	var wind = math.sin(self.tick * 0.02) * 0.25
	for i : 0 .. self.fx.size() - 1
	  self.fy[i] += self.fs[i]
	  self.fx[i] += wind + math.sin(self.tick * 0.1 + i) * 0.12
	  if self.fx[i] < 0
		self.fx[i] += width()
	  elif self.fx[i] >= width()
		self.fx[i] -= width()
	  end
	  var col = int(self.fx[i])
	  if self.fy[i] >= height() - self.pile[col]
		if self.pile[col] < 5
		  self.pile[col] += 1
		end
		self.fy[i] = 0.0
		self.fx[i] = (math.rand() % 520) / 10.0
	  end
	end
	# Slow thaw.
	if self.tick % 6 == 0
	  var c = math.rand() % width()
	  if self.pile[c] > 0
		self.pile[c] -= 1
	  end
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 50
	  self.last = t
	  self.step()
	end
	clear(rgb(4, 8, 22))
	for x : 0 .. width() - 1
	  if self.pile[x] > 0
		line(x, height() - 1, x, height() - self.pile[x], rgb(200, 215, 240))
	  end
	end
	for i : 0 .. self.fx.size() - 1
	  var big = self.fs[i] > 0.28
	  pixel(int(self.fx[i]), int(self.fy[i]), big ? rgb(255, 255, 255) : rgb(150, 170, 210))
	end
  end
end

return App()
