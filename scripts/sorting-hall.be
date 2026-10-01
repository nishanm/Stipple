# name: Sorting Hall
# summary: Watch insertion sort put a shuffled row of bars in order, one comparison at a time. Then it shuffles and starts again.
# author: Stipple
# tags: animation, algorithm, geek
# panel: 52x16

import math

class App
  var a
  var i
  var j
  var mode
  var hold
  var last

  def init()
	self.last = now_ms()
	self.shuffle()
  end

  def shuffle()
	self.a = []
	for k : 0 .. 25
	  self.a.push(1 + (k * 15) / 25)
	end
	for k : 0 .. 24
	  var r = k + math.rand() % (26 - k)
	  var tmp = self.a[k]
	  self.a[k] = self.a[r]
	  self.a[r] = tmp
	end
	self.i = 1
	self.j = 1
	self.mode = 0
	self.hold = 0
  end

  def step()
	if self.mode == 1
	  # Sorted: hold the green sweep for a moment.
	  self.hold += 1
	  if self.hold > 40
		self.shuffle()
	  end
	  return
	end
	for n : 0 .. 1
	  if self.j > 0 && self.a[self.j - 1] > self.a[self.j]
		var tmp = self.a[self.j]
		self.a[self.j] = self.a[self.j - 1]
		self.a[self.j - 1] = tmp
		self.j -= 1
	  else
		self.i += 1
		self.j = self.i
		if self.i >= self.a.size()
		  self.mode = 1
		  return
		end
	  end
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 45
	  self.last = t
	  self.step()
	end
	clear(rgb(0, 0, 0))
	for k : 0 .. self.a.size() - 1
	  var h = self.a[k]
	  var col = rgb(30, 90 + h * 10, 255 - h * 8)
	  if self.mode == 1
		var sweep = self.hold * 2
		col = k < sweep ? rgb(60, 255, 100) : col
	  elif k == self.j
		col = rgb(255, 80, 80)
	  elif k < self.i
		col = rgb(60, 160, 255)
	  end
	  rect_fill(k * 2, 16 - h, 2, h, col)
	end
  end
end

return App()
