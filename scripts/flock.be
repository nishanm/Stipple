# name: Flock
# summary: Eight boids follow three simple rules - stay together, match heading, keep apart - and behave like a flock.
# author: Stipple
# tags: animation, ai, ambient
# panel: 52x16

import math

class App
  var px
  var py
  var vx
  var vy
  var last

  def init()
	self.px = []
	self.py = []
	self.vx = []
	self.vy = []
	self.last = now_ms()
	for i : 0 .. 7
	  self.px.push(10.0 + math.rand() % 30)
	  self.py.push(3.0 + math.rand() % 10)
	  self.vx.push((math.rand() % 100) / 60.0 - 0.8)
	  self.vy.push((math.rand() % 100) / 120.0 - 0.4)
	end
  end

  def step()
	var n = self.px.size()
	for i : 0 .. n - 1
	  var cx = 0.0
	  var cy = 0.0
	  var ax = 0.0
	  var ay = 0.0
	  var sx = 0.0
	  var sy = 0.0
	  var seen = 0
	  for j : 0 .. n - 1
		if j != i
		  var dx = self.px[j] - self.px[i]
		  var dy = self.py[j] - self.py[i]
		  var d2 = dx * dx + dy * dy
		  if d2 < 400
			seen += 1
			cx += self.px[j]
			cy += self.py[j]
			ax += self.vx[j]
			ay += self.vy[j]
			if d2 < 14
			  sx -= dx
			  sy -= dy
			end
		  end
		end
	  end
	  if seen > 0
		self.vx[i] += (cx / seen - self.px[i]) * 0.006 + (ax / seen - self.vx[i]) * 0.05 + sx * 0.04
		self.vy[i] += (cy / seen - self.py[i]) * 0.006 + (ay / seen - self.vy[i]) * 0.05 + sy * 0.04
	  end

	  # Turn back before the edge rather than bouncing off it.
	  if self.px[i] < 5
		self.vx[i] += 0.12
	  elif self.px[i] > 46
		self.vx[i] -= 0.12
	  end
	  if self.py[i] < 3
		self.vy[i] += 0.12
	  elif self.py[i] > 12
		self.vy[i] -= 0.12
	  end

	  var sp = math.sqrt(self.vx[i] * self.vx[i] + self.vy[i] * self.vy[i])
	  if sp > 1.1
		self.vx[i] = self.vx[i] / sp * 1.1
		self.vy[i] = self.vy[i] / sp * 1.1
	  elif sp < 0.35 && sp > 0.0
		self.vx[i] = self.vx[i] / sp * 0.35
		self.vy[i] = self.vy[i] / sp * 0.35
	  end
	end
	for i : 0 .. n - 1
	  self.px[i] += self.vx[i]
	  self.py[i] += self.vy[i]
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 45
	  self.last = t
	  self.step()
	end
	clear(rgb(0, 4, 12))
	for i : 0 .. self.px.size() - 1
	  var x = int(self.px[i])
	  var y = int(self.py[i])
	  pixel(int(self.px[i] - self.vx[i] * 2), int(self.py[i] - self.vy[i] * 2), rgb(20, 60, 90))
	  pixel(int(self.px[i] - self.vx[i]), int(self.py[i] - self.vy[i]), rgb(50, 130, 190))
	  pixel(x, y, rgb(230, 250, 255))
	end
  end
end

return App()
