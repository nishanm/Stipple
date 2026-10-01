# name: Savanna Sunset
# summary: The sun sinks behind the acacias and the sky burns from gold to violet. Stars come out, then dawn starts it again. Press to send a bird across.
# author: Stipple
# tags: ambient, nature, relaxing, button
# panel: 52x16

# One 150 second loop, so somebody glancing at the panel sees the sky moving
# rather than waiting for the real sunset. The phase p runs 0..1: the sun
# descends until 0.7, night follows, and it rises again from 0.9.

import math

class App
  var last, tk, tp, p
  var birdOn, birdX, birdY
  var stx, sty

  def init()
	self.stx = [3, 9, 15, 20, 26, 31, 36, 41, 47, 5, 23, 34, 44, 12, 49]
	self.sty = [1, 3, 0, 5, 2, 0, 4, 1, 3, 6, 7, 6, 6, 4, 7]
	self.birdOn = false
	self.birdX = 0.0
	self.birdY = 4
	self.last = 0
	self.tk = 0
	self.tp = 0.0
	self.p = 0.0
  end

  def mix(a, b, t)
	var r = int(((a >> 16) & 255) * (1.0 - t) + ((b >> 16) & 255) * t)
	var g = int(((a >> 8) & 255) * (1.0 - t) + ((b >> 8) & 255) * t)
	var bl = int((a & 255) * (1.0 - t) + (b & 255) * t)
	return rgb(r, g, bl)
  end

  def clamp(v, lo, hi)
	if v < lo
	  return lo
	end
	if v > hi
	  return hi
	end
	return v
  end

  def darkness(p)
	if p < 0.7
	  return p / 0.7 * 0.85
	elif p < 0.9
	  return 0.85 + (p - 0.7) / 0.2 * 0.15
	end
	return 1.0 - (p - 0.9) / 0.1
  end

  def on_button(name)
	if !self.birdOn
	  self.birdOn = true
	  self.birdX = -3.0
	  self.birdY = 3 + math.rand() % 4
	end
  end

  def step()
	self.tk += 1
	if self.birdOn
	  self.birdX += 0.3
	  if self.birdX > 56.0
		self.birdOn = false
	  end
	elif self.p < 0.75 && math.rand() % 900 == 0
	  self.on_button("")
	end
  end

  def tree(x, h, w, k)
	var trunk = self.mix(0x5A3A1A, 0x050304, k)
	var leaf = self.mix(0x3A6A20, 0x050604, k)
	var sw = math.sin(self.tp * 0.8 + x) > 0.5 ? 1 : 0
	var ty = 12 - h
	line(x, 12, x, ty + 1, trunk)
	pixel(x - 1, ty + 1, trunk)
	pixel(x + 1, ty + 1, trunk)
	rect_fill(x - w / 2 + sw, ty, w, 1, leaf)
	rect_fill(x - w / 2 + 2 + sw, ty - 1, w - 4, 1, leaf)
  end

  def draw_sky(p)
	var top = 0
	var hor = 0
	if p < 0.7
	  var u = p / 0.7
	  top = self.mix(0x3A78C8, 0x4A2A78, u)
	  hor = self.mix(0xFFD070, 0xFF4A18, u)
	elif p < 0.85
	  var u = (p - 0.7) / 0.15
	  top = self.mix(0x4A2A78, 0x040614, u)
	  hor = self.mix(0xFF4A18, 0x2A1038, u)
	else
	  var u = (p - 0.85) / 0.15
	  top = self.mix(0x040614, 0x3A78C8, u)
	  hor = self.mix(0x2A1038, 0xFFD070, u)
	end
	for y : 0 .. 12
	  var t = y / 12.0
	  rect_fill(0, y, 52, 1, self.mix(top, hor, t * t))
	end

	var nf = 0.0
	if p > 0.72
	  nf = self.clamp((p - 0.72) / 0.13, 0.0, 1.0) * self.clamp((0.97 - p) / 0.07, 0.0, 1.0)
	end
	if nf > 0.0
	  for i : 0 .. size(self.stx) - 1
		var v = int(nf * (140 + 100 * math.sin(self.tp * 2.0 + i)))
		if v > 8
		  pixel(self.stx[i], self.sty[i], rgb(v, v, int(v * 0.9)))
		end
	  end
	end

	var sy = 99
	if p < 0.7
	  sy = 3 + int(p / 0.7 * 11)
	elif p >= 0.9
	  sy = 15 - int((p - 0.9) / 0.1 * 12)
	end
	if sy < 15
	  var sun = self.mix(0xFFF0A0, 0xFF6020, self.clamp(p / 0.7, 0.0, 1.0))
	  rect_fill(30, sy - 1, 7, 3, self.mix(hor, sun, 0.35))
	  rect_fill(32, sy - 2, 3, 5, sun)
	  rect_fill(31, sy - 1, 5, 3, sun)
	end
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 33
	  self.last = t
	  self.step()
	end
	self.tp = t / 1000.0
	self.p = (t % 150000) / 150000.0
	var p = self.p
	var k = self.darkness(p)

	self.draw_sky(p)

	rect_fill(0, 13, 52, 3, self.mix(0x5A4A20, 0x060406, k))
	var grass = self.mix(0x6A7A28, 0x060806, k)
	for i : 0 .. 10
	  var x = 2 + i * 5
	  pixel(x + (math.sin(self.tp * 1.5 + x) > 0.3 ? 1 : 0), 12, grass)
	end
	self.tree(12, 4, 11, k)
	self.tree(44, 3, 7, k)

	if self.birdOn
	  var flap = int(self.tp * 5.0) % 2
	  var bc = self.mix(0x2A2A30, 0x000000, k)
	  var bx = int(self.birdX)
	  pixel(bx, self.birdY, bc)
	  pixel(bx - 1, self.birdY - flap, bc)
	  pixel(bx + 1, self.birdY - flap, bc)
	end
  end
end

return App()
