# name: Pond
# summary: A frog on a lily pad snaps at a dragonfly while raindrops ripple the water and the reeds sway. Press to make it hop.
# author: Stipple
# tags: ambient, nature, animation, button
# panel: 52x16

# Seen from above. The frog waits on a pad and flicks its tongue at the
# dragonfly when it wanders close, hops to the other pad now and then, and
# every landing throws a ripple. The dragonfly wanders on a slowly turning
# heading, so it never follows a path.

import math

class App
  var last, tk, tp
  var padX, padY
  var frog, fx, fy, jt, jfrom, jto
  var flyOn, flyX, flyY, flyA, flyWait
  var tt, tx, ty, cool
  var rx, ry, rr, ron

  def init()
	self.padX = [14, 38]
	self.padY = [8, 6]
	self.frog = 0
	self.fx = 14.0
	self.fy = 8.0
	self.jt = 0
	self.jfrom = 0
	self.jto = 1
	self.flyOn = true
	self.flyX = 30.0
	self.flyY = 4.0
	self.flyA = 3.5
	self.flyWait = 0
	self.tt = 0
	self.tx = 0.0
	self.ty = 0.0
	self.cool = 0
	self.rx = [0, 0, 0, 0, 0]
	self.ry = [0, 0, 0, 0, 0]
	self.rr = [0.0, 0.0, 0.0, 0.0, 0.0]
	self.ron = [false, false, false, false, false]
	self.last = 0
	self.tk = 0
	self.tp = 0.0
  end

  def mix(a, b, t)
	var r = int(((a >> 16) & 255) * (1.0 - t) + ((b >> 16) & 255) * t)
	var g = int(((a >> 8) & 255) * (1.0 - t) + ((b >> 8) & 255) * t)
	var bl = int((a & 255) * (1.0 - t) + (b & 255) * t)
	return rgb(r, g, bl)
  end

  def ripple(x, y)
	for i : 0 .. 4
	  if !self.ron[i]
		self.rx[i] = x
		self.ry[i] = y
		self.rr[i] = 0.5
		self.ron[i] = true
		return nil
	  end
	end
  end

  def on_button(name)
	if self.jt == 0 && self.tt == 0
	  self.jump()
	end
  end

  def jump()
	self.jfrom = self.frog
	self.jto = 1 - self.frog
	self.jt = 24
  end

  def dist(ax, ay, bx, by)
	return math.sqrt((ax - bx) * (ax - bx) + (ay - by) * (ay - by))
  end

  def step()
	self.tk += 1

	for i : 0 .. 4
	  if self.ron[i]
		self.rr[i] += 0.08
		if self.rr[i] > 6.0
		  self.ron[i] = false
		end
	  end
	end
	if math.rand() % 45 == 0
	  self.ripple(math.rand() % 52, math.rand() % 16)
	end

	if self.jt > 0
	  self.jt -= 1
	  var u = 1.0 - self.jt / 24.0
	  self.fx = self.padX[self.jfrom] + (self.padX[self.jto] - self.padX[self.jfrom]) * u
	  self.fy = self.padY[self.jfrom] + (self.padY[self.jto] - self.padY[self.jfrom]) * u
	  if self.jt == 0
		self.frog = self.jto
		self.ripple(self.padX[self.frog], self.padY[self.frog])
		self.cool = 40
	  end
	end

	if self.flyOn
	  self.flyA += ((math.rand() % 100) - 50) / 400.0
	  self.flyX += math.cos(self.flyA) * 0.25
	  self.flyY += math.sin(self.flyA) * 0.25
	  if self.flyX < 3.0 || self.flyX > 48.0
		self.flyA = 3.14159 - self.flyA
		self.flyX = self.flyX < 3.0 ? 3.0 : 48.0
	  end
	  if self.flyY < 1.0 || self.flyY > 14.0
		self.flyA = 0.0 - self.flyA
		self.flyY = self.flyY < 1.0 ? 1.0 : 14.0
	  end
	else
	  self.flyWait -= 1
	  if self.flyWait <= 0
		self.flyOn = true
		self.flyX = 2.0
		self.flyY = 2.0 + math.rand() % 12
		self.flyA = 0.0
	  end
	end

	if self.cool > 0
	  self.cool -= 1
	end
	if self.tt > 0
	  self.tt -= 1
	  if self.tt == 5 && self.flyOn && self.dist(self.flyX, self.flyY, self.tx, self.ty) <= 3.0
		self.flyOn = false
		self.flyWait = 200
	  end
	elif self.jt == 0 && self.cool == 0 && self.flyOn && self.dist(self.flyX, self.flyY, self.fx, self.fy) < 9.0 && math.rand() % 12 == 0
	  self.tt = 10
	  self.tx = self.flyX
	  self.ty = self.flyY
	  self.cool = 60
	elif self.jt == 0 && self.tt == 0 && math.rand() % 600 == 0
	  self.jump()
	end
  end

  def draw_pad(i)
	var cx = self.padX[i]
	var cy = self.padY[i]
	for dy : -2 .. 2
	  var hw = (dy == 2 || dy == -2) ? 2 : 4
	  rect_fill(cx - hw, cy + dy, hw * 2 + 1, 1, 0x2E9A3A)
	  pixel(cx - hw, cy + dy, 0x1E7A2A)
	  pixel(cx + hw, cy + dy, 0x1E7A2A)
	end
	pixel(cx + 3, cy - 1, 0x0E4050)
	pixel(cx + 4, cy - 1, 0x0E4050)
	if i == 1
	  pixel(cx - 2, cy, 0xFFA0C0)
	  pixel(cx - 3, cy, 0xE070A0)
	  pixel(cx - 1, cy, 0xE070A0)
	  pixel(cx - 2, cy - 1, 0xE070A0)
	  pixel(cx - 2, cy + 1, 0xE070A0)
	end
  end

  def draw_reed(x, h)
	for n : 0 .. h - 1
	  var off = int(math.sin(self.tp * 1.2 + x * 0.7 - n * 0.5) * n * 0.25 + 0.5)
	  pixel(x + off, 15 - n, 0x2A6A30)
	end
	pixel(x + int(math.sin(self.tp * 1.2 + x * 0.7 - h * 0.5) * h * 0.25 + 0.5), 15 - h, 0x7A4A20)
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 33
	  self.last = t
	  self.step()
	end
	self.tp = t / 1000.0

	for y : 0 .. 15
	  rect_fill(0, y, 52, 1, self.mix(0x0E4A5A, 0x0A3644, y / 15.0))
	end

	for i : 0 .. 4
	  if self.ron[i]
		var c = self.mix(0x4AA8B8, 0x0E4050, self.rr[i] / 6.0)
		var n = int(self.rr[i] * 4.0) + 4
		for k : 0 .. n - 1
		  var a = k * 6.2832 / n
		  pixel(self.rx[i] + int(math.cos(a) * self.rr[i] * 1.6), self.ry[i] + int(math.sin(a) * self.rr[i] * 0.8), c)
		end
	  end
	end

	self.draw_pad(0)
	self.draw_pad(1)
	self.draw_reed(1, 5)
	self.draw_reed(3, 4)
	self.draw_reed(49, 6)
	self.draw_reed(50, 4)

	var fyy = self.fy
	if self.jt > 0
	  fyy = self.fy - math.sin(3.14159 * (1.0 - self.jt / 24.0)) * 4.0
	end
	var frx = int(self.fx)
	var fry = int(fyy)
	if self.tt > 0
	  var u = self.tt > 5 ? (10 - self.tt) / 5.0 : self.tt / 5.0
	  line(frx, fry, frx + int((self.tx - self.fx) * u), fry + int((self.ty - self.fy) * u), 0xFF6A8A)
	end
	rect_fill(frx - 1, fry, 3, 2, 0x60D040)
	pixel(frx - 1, fry - 1, 0xFFF080)
	pixel(frx + 1, fry - 1, 0xFFF080)
	pixel(frx - 2, fry + 1, 0x40A030)
	pixel(frx + 2, fry + 1, 0x40A030)

	if self.flyOn
	  var fxx = int(self.flyX)
	  var fyy2 = int(self.flyY)
	  pixel(fxx, fyy2, 0x40FFD0)
	  pixel(fxx - 1, fyy2, 0x20B090)
	  var w = self.tk % 2
	  pixel(fxx, fyy2 - 1 + w * 2, 0xC0F0FF)
	end
  end
end

return App()
