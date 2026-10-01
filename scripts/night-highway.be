# name: Night Highway
# summary: Cars stream both ways along a dark road under the stars, headlights glowing, lane markings sliding past.
# author: Stipple
# tags: animation, ambient, night, cars
# panel: 52x16

import math

class App
  var cx
  var lane
  var spd
  var col
  var off
  var last

  def init()
	self.cx = []
	self.lane = []
	self.spd = []
	self.col = []
	self.off = 0
	self.last = now_ms()
  end

  def spawn()
	var lane = math.rand() % 2
	var x = lane == 0 ? -6.0 : 52.0
	# Refuse a spawn on top of another car.
	for i : 0 .. self.cx.size() - 1
	  if self.lane[i] == lane && math.abs(self.cx[i] - x) < 12
		return
	  end
	end
	var pal = [rgb(200, 40, 40), rgb(50, 90, 200), rgb(180, 180, 190), rgb(220, 160, 30), rgb(60, 160, 90)]
	self.cx.push(x)
	self.lane.push(lane)
	self.spd.push((lane == 0 ? 1 : -1) * (0.5 + (math.rand() % 10) / 12.0))
	self.col.push(pal[math.rand() % 5])
  end

  def step()
	self.off = (self.off + 1) % 8
	var i = 0
	while i < self.cx.size()
	  self.cx[i] += self.spd[i]
	  if self.cx[i] < -8 || self.cx[i] > 60
		self.cx.pop(i)
		self.lane.pop(i)
		self.spd.pop(i)
		self.col.pop(i)
	  else
		i += 1
	  end
	end
	if (math.rand() % 8) == 0 && self.cx.size() < 6
	  self.spawn()
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 45
	  self.last = t
	  self.step()
	end
	clear(rgb(2, 2, 10))
	for s : 0 .. 11
	  pixel((s * 17 + 5) % 52, (s * 5) % 3, rgb(90, 90, 120))
	end
	rect_fill(0, 3, 52, 10, rgb(16, 16, 20))
	for x : 0 .. 51
	  if (x + self.off) % 8 < 4
		pixel(x, 7, rgb(120, 100, 20))
	  end
	end
	for i : 0 .. self.cx.size() - 1
	  var x = int(self.cx[i])
	  var y = self.lane[i] == 0 ? 4 : 9
	  var right = self.lane[i] == 0
	  rect_fill(x, y, 5, 2, self.col[i])
	  var head = right ? x + 5 : x - 1
	  var tail = right ? x - 1 : x + 5
	  pixel(head, y, rgb(255, 255, 200))
	  pixel(head, y + 1, rgb(255, 255, 200))
	  pixel(right ? head + 1 : head - 1, y, rgb(90, 90, 50))
	  pixel(right ? head + 2 : head - 2, y, rgb(50, 50, 30))
	  pixel(tail, y, rgb(200, 20, 20))
	  pixel(tail, y + 1, rgb(200, 20, 20))
	end
  end
end

return App()
