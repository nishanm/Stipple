# name: Night Train
# summary: A little train rattles across the countryside past telegraph poles, with hills and mountains sliding by at different speeds. Lit windows come on after dark.
# author: Stipple
# tags: ambient, animation, clock
# panel: 52x16

# The train stays put and the world moves. Each layer scrolls by its own
# fraction of the same offset - clouds slowest, then mountains, hills, and the
# poles at full speed - which is all the depth there is. After 21:00 and
# before 06:00 the palette dims and the windows light up; with no clock it is
# always day.

import math

class App
  var last, tk, tp, off, nt
  var smx, smy, sml, smi

  def init()
	self.smx = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
	self.smy = [0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
	self.sml = [0, 0, 0, 0, 0, 0, 0, 0]
	self.smi = 0
	self.off = 0.0
	self.last = 0
	self.tk = 0
	self.tp = 0.0
	self.nt = false
  end

  def night()
	if !time_known()
	  return false
	end
	var h = hour()
	return h >= 21 || h < 6
  end

  def col(c)
	if !self.nt
	  return c
	end
	return rgb(int(((c >> 16) & 255) * 0.28), int(((c >> 8) & 255) * 0.28), int((c & 255) * 0.32))
  end

  def mix(a, b, t)
	var r = int(((a >> 16) & 255) * (1.0 - t) + ((b >> 16) & 255) * t)
	var g = int(((a >> 8) & 255) * (1.0 - t) + ((b >> 8) & 255) * t)
	var bl = int((a & 255) * (1.0 - t) + (b & 255) * t)
	return rgb(r, g, bl)
  end

  def step()
	self.tk += 1
	self.off += 0.6
	if self.tk % 6 == 0
	  var i = self.smi
	  self.smx[i] = 49.0
	  self.smy[i] = 6.0
	  self.sml[i] = 40
	  self.smi = (i + 1) % 8
	end
	for i : 0 .. 7
	  if self.sml[i] > 0
		self.sml[i] -= 1
		self.smx[i] -= 0.35
		self.smy[i] -= 0.1
	  end
	end
  end

  def draw_car(x, body, win)
	rect_fill(x, 8, 12, 5, body)
	rect_fill(x, 8, 12, 1, self.mix(body, 0x000000, 0.35))
	for k : 0 .. 2
	  rect_fill(x + 1 + k * 4, 9, 2, 2, win)
	end
	rect_fill(x, 11, 12, 1, self.mix(body, 0x000000, 0.2))
	pixel(x + 12, 11, 0x202020)
	pixel(x + 2, 13, 0x202020)
	pixel(x + 3, 13, 0x202020)
	pixel(x + 8, 13, 0x202020)
	pixel(x + 9, 13, 0x202020)
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 33
	  self.last = t
	  self.step()
	end
	self.tp = t / 1000.0
	self.nt = self.night()
	var win = self.nt ? 0xFFD070 : 0x9AD0F0

	for y : 0 .. 12
	  rect_fill(0, y, 52, 1, self.col(self.mix(0x4A9AE0, 0xB0D8F0, y / 12.0)))
	end
	if self.nt
	  pixel(46, 2, 0xE0E8FF)
	  pixel(47, 2, 0xE0E8FF)
	  pixel(46, 3, 0xE0E8FF)
	  pixel(47, 3, 0xA0A8C0)
	else
	  rect_fill(45, 1, 3, 3, 0xFFE070)
	end

	var cloud = self.col(0xF0F4FA)
	for c : 0 .. 1
	  var cx = 60 - int(self.off * 0.05 + c * 34) % 78
	  rect_fill(cx, 3 + c, 6, 1, cloud)
	  rect_fill(cx + 1, 2 + c, 3, 1, cloud)
	end

	var mo = self.off * 0.1
	var mtn = self.col(0x6A7A9A)
	for x : 0 .. 51
	  var h = int(math.sin((x + mo) * 0.11) * 2.5 + math.sin((x + mo) * 0.27) * 1.2)
	  var top = 6 + h
	  rect_fill(x, top, 1, 13 - top, mtn)
	end
	var ho = self.off * 0.3
	var hill = self.col(0x2E7A3A)
	for x : 0 .. 51
	  var top = 10 + int(math.sin((x + ho) * 0.2) * 1.5)
	  rect_fill(x, top, 1, 13 - top, hill)
	end

	var pole = self.col(0x4A3A28)
	var po = int(self.off) % 26
	for i : 0 .. 2
	  var x = i * 26 - po
	  line(x, 5, x, 12, pole)
	  line(x - 1, 5, x + 1, 5, pole)
	end

	rect_fill(0, 13, 52, 1, self.col(0x9098A0))
	rect_fill(0, 14, 52, 2, self.col(0x5A4028))
	var sl = int(self.off) % 4
	for x : 0 .. 12
	  pixel(x * 4 - sl, 14, self.col(0x3A2A18))
	end

	self.draw_car(0, self.col(0x2A6AB0), win)
	self.draw_car(13, self.col(0xC08A20), win)
	self.draw_car(26, self.col(0x2A6AB0), win)
	self.draw_car(39, self.col(0x333A48), win)
	rect_fill(39, 7, 4, 1, self.col(0x333A48))
	rect_fill(48, 6, 2, 3, self.col(0x222630))
	pixel(51, 11, 0xFFF0A0)

	for i : 0 .. 7
	  if self.sml[i] > 0
		var v = 60 + self.sml[i] * 4
		if self.nt
		  v = v / 3
		end
		pixel(int(self.smx[i]), int(self.smy[i]), rgb(v, v, v))
	  end
	end
  end
end

return App()
