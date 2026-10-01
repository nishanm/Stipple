# name: Snow Globe
# summary: A cosy cottage under a swirl of snow that slowly settles into drifts. Press to shake the globe.
# author: Stipple
# tags: ambient, seasonal, animation, button
# panel: 52x16

# Forty flakes, each with its own velocity. A flake that reaches the ground
# stops being a particle and adds to the height of its column, so the drifts
# build up as the snow falls. A shake flings the flakes back up and levels the
# drifts. If the globe sits quiet for a while it shakes itself.

import math

class App
  var px, py, pvx, pvy, pon
  var heap
  var last, tk, tp, quiet

  def init()
	self.px = []
	self.py = []
	self.pvx = []
	self.pvy = []
	self.pon = []
	for i : 0 .. 39
	  self.px.push(0.0)
	  self.py.push(0.0)
	  self.pvx.push(0.0)
	  self.pvy.push(0.0)
	  self.pon.push(false)
	end
	self.heap = []
	for x : 0 .. 51
	  self.heap.push(0)
	end
	self.last = 0
	self.tk = 0
	self.tp = 0.0
	self.quiet = 0
	self.shake()
  end

  def shake()
	for i : 0 .. 39
	  self.px[i] = (math.rand() % 520) / 10.0
	  self.py[i] = 4.0 + (math.rand() % 80) / 10.0
	  self.pvx[i] = ((math.rand() % 100) - 50) / 60.0
	  self.pvy[i] = 0.0 - (math.rand() % 100) / 70.0
	  self.pon[i] = true
	end
	for x : 0 .. 51
	  self.heap[x] = 0
	end
	self.quiet = 0
  end

  def on_button(name)
	self.shake()
  end

  def step()
	self.tk += 1
	var live = 0
	for i : 0 .. 39
	  if self.pon[i]
		live += 1
		self.pvy[i] += 0.012
		if self.pvy[i] > 0.2
		  self.pvy[i] = 0.2
		end
		self.pvx[i] = self.pvx[i] * 0.985 + math.sin(self.tk * 0.05 + self.py[i]) * 0.004
		self.px[i] += self.pvx[i]
		self.py[i] += self.pvy[i]
		if self.px[i] < 0.0
		  self.px[i] = 0.0
		  self.pvx[i] = math.abs(self.pvx[i])
		elif self.px[i] > 51.0
		  self.px[i] = 51.0
		  self.pvx[i] = 0.0 - math.abs(self.pvx[i])
		end
		if self.py[i] < 0.0
		  self.py[i] = 0.0
		  self.pvy[i] = 0.02
		end
		var gx = int(self.px[i])
		if int(self.py[i]) >= 13 - self.heap[gx]
		  self.pon[i] = false
		  if math.rand() % 3 == 0 && self.heap[gx] < 3
			self.heap[gx] += 1
		  end
		end
	  end
	end
	if live == 0
	  self.quiet += 1
	  if self.quiet > 750
		self.shake()
	  end
	end
  end

  def pine(cx, top, rows)
	for r : 0 .. rows - 1
	  var hw = r / 2
	  rect_fill(cx - hw, top + r, hw * 2 + 1, 1, r % 2 == 0 ? 0x1A5A2A : 0x14481F)
	end
	pixel(cx, top, 0xE8F0FF)
	pixel(cx, top + rows, 0x4A2A14)
  end

  def draw_house()
	rect_fill(22, 9, 8, 4, 0x7A3A24)
	for i : 0 .. 3
	  rect_fill(21 + i, 8 - i, 10 - i * 2, 1, 0xE8E8F0)
	end
	rect_fill(23, 11, 2, 2, 0x3A2010)
	var glow = (self.tk / 8) % 7 == 0 ? 0xFFC050 : 0xFFE080
	rect_fill(27, 10, 2, 2, glow)
	rect_fill(21, 12, 10, 1, 0xC8D0E0)
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 33
	  self.last = t
	  self.step()
	end
	self.tp = t / 1000.0

	rect_fill(0, 0, 52, 5, 0x0A1230)
	rect_fill(0, 5, 52, 5, 0x102040)
	rect_fill(0, 10, 52, 3, 0x18305A)
	rect_fill(44, 2, 2, 2, 0xE8ECFF)
	pixel(43, 2, 0x606880)

	self.pine(7, 6, 6)
	self.pine(38, 8, 5)
	self.pine(45, 7, 5)
	self.draw_house()

	rect_fill(0, 13, 52, 1, 0xE8F0FF)
	rect_fill(0, 14, 52, 2, 0x5A3A1E)
	for x : 0 .. 51
	  var h = self.heap[x]
	  if h > 0
		rect_fill(x, 13 - h, 1, h, 0xE8F0FF)
	  end
	end

	for i : 0 .. 39
	  if self.pon[i]
		pixel(int(self.px[i]), int(self.py[i]), i % 2 == 0 ? 0xFFFFFF : 0xB8C8E8)
	  end
	end
  end
end

return App()
