# name: Rubber Balls
# summary: Six bouncing balls with real gravity and energy loss. When one settles, it is thrown again.
# author: Stipple
# tags: animation, physics, colourful
# panel: 52x16

import math

class App
  var x
  var y
  var vx
  var vy
  var col
  var last

  def init()
	self.x = []
	self.y = []
	self.vx = []
	self.vy = []
	self.col = [rgb(255, 80, 80), rgb(255, 180, 50), rgb(90, 230, 120), rgb(70, 200, 255), rgb(180, 110, 255), rgb(255, 120, 200)]
	self.last = now_ms()
	for i : 0 .. 5
	  self.x.push(4.0 + i * 8)
	  self.y.push(2.0 + (math.rand() % 6))
	  self.vx.push((math.rand() % 100) / 100.0 - 0.5)
	  self.vy.push(0.0)
	end
  end

  def throw(i)
	self.y[i] = 12.0
	self.vy[i] = -(1.4 + (math.rand() % 10) / 10.0)
	self.vx[i] = (math.rand() % 100) / 60.0 - 0.8
  end

  def step()
	for i : 0 .. 5
	  self.vy[i] += 0.11
	  self.y[i] += self.vy[i]
	  self.x[i] += self.vx[i]
	  if self.x[i] < 1
		self.x[i] = 1.0
		self.vx[i] = -self.vx[i]
	  elif self.x[i] > 49
		self.x[i] = 49.0
		self.vx[i] = -self.vx[i]
	  end
	  if self.y[i] > 13
		self.y[i] = 13.0
		self.vy[i] = -self.vy[i] * 0.82
		if self.vy[i] > -0.35
		  self.throw(i)
		end
	  end
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 30
	  self.last = t
	  self.step()
	end
	clear(rgb(6, 6, 14))
	line(0, 15, 51, 15, rgb(40, 40, 70))
	for i : 0 .. 5
	  var x = int(self.x[i])
	  var y = int(self.y[i])
	  # Shadow gets smaller the higher the ball is.
	  var sw = y > 8 ? 2 : 1
	  rect_fill(x, 14, sw, 1, rgb(20, 20, 34))
	  # A short motion streak.
	  pixel(x, y - int(self.vy[i] * 2), rgb(30, 30, 50))
	  rect_fill(x, y, 2, 2, self.col[i])
	end
  end
end

return App()
