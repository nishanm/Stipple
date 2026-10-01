# name: Planet Flyby
# summary: A banded planet rolls past a drifting starfield while a small ship cruises alongside. Press to jump to warp.
# author: Stipple
# tags: ambient, space, animation, button
# panel: 52x16

# The planet is drawn per pixel: each row of the disc is shaded from a band
# pattern that slides sideways (the spin) and darkened toward the right, away
# from the light. Stars carry their own speed, so the near ones outrun the far
# ones and the field has depth. A button press ramps every star into a streak.

import math

class App
  var sx, sy, ss
  var last, tk, tp
  var warp, boost
  var px, pal, pals

  def init()
	self.sx = []
	self.sy = []
	self.ss = []
	for i : 0 .. 34
	  self.sx.push((math.rand() % 520) / 10.0)
	  self.sy.push(math.rand() % 16)
	  self.ss.push(0.04 + (math.rand() % 30) / 100.0)
	end
	self.pals = [[0xE0A060, 0x9A5A2A], [0x5AA0F0, 0x2A50A0], [0xD04A38, 0x7A2418], [0x70D0A0, 0x2A7A58]]
	self.pal = 0
	self.px = 40.0
	self.last = 0
	self.tk = 0
	self.tp = 0.0
	self.warp = 0
	self.boost = 1.0
  end

  def mix(a, b, t)
	var r = int(((a >> 16) & 255) * (1.0 - t) + ((b >> 16) & 255) * t)
	var g = int(((a >> 8) & 255) * (1.0 - t) + ((b >> 8) & 255) * t)
	var bl = int((a & 255) * (1.0 - t) + (b & 255) * t)
	return rgb(r, g, bl)
  end

  def dim(c, f)
	return rgb(int(((c >> 16) & 255) * f), int(((c >> 8) & 255) * f), int((c & 255) * f))
  end

  def on_button(name)
	self.warp = 90
  end

  def step()
	self.tk += 1
	self.boost = 1.0
	if self.warp > 0
	  self.warp -= 1
	  var ramp = 1.0
	  if self.warp > 70
		ramp = (90 - self.warp) / 20.0
	  elif self.warp < 20
		ramp = self.warp / 20.0
	  end
	  self.boost = 1.0 + ramp * 14.0
	end
	for i : 0 .. size(self.sx) - 1
	  self.sx[i] -= self.ss[i] * self.boost
	  if self.sx[i] < -8.0
		self.sx[i] = 52.0 + (math.rand() % 8)
		self.sy[i] = math.rand() % 16
	  end
	end
	self.px -= 0.07 * (self.warp > 0 ? self.boost * 0.5 : 1.0)
	if self.px < -14.0
	  self.px = 66.0 + (math.rand() % 40)
	  self.pal = math.rand() % 4
	end
  end

  def draw_stars()
	for i : 0 .. size(self.sx) - 1
	  var v = int(70 + self.ss[i] * 400)
	  var c = rgb(v, v, v)
	  if self.boost > 2.0
		c = rgb(int(v * 0.7), int(v * 0.85), v)
	  end
	  var x = int(self.sx[i])
	  var len = int(self.ss[i] * self.boost * 1.2)
	  if len > 0
		line(x, self.sy[i], x + len, self.sy[i], c)
	  else
		pixel(x, self.sy[i], c)
	  end
	end
  end

  def draw_planet()
	var r = 6
	var cx = int(self.px)
	var cy = 8
	var a = self.pals[self.pal][0]
	var b = self.pals[self.pal][1]
	var lo = 0 - r
	for dy : lo .. r
	  var hw = int(math.sqrt(r * r - dy * dy) + 0.5)
	  for dx : (0 - hw) .. hw
		var u = dx * 1.0 / r
		var band = math.sin(dy * 0.9 + math.sin(u * 1.4 + self.tp * 0.4) * 0.8)
		var f = 1.0 - 0.7 * (dx + r) / (2.0 * r)
		pixel(cx + dx, cy + dy, self.dim(self.mix(a, b, (band + 1.0) / 2.0), f))
	  end
	end
	if self.pal == 0
	  line(cx - 10, cy + 3, cx + 10, cy - 3, 0xD8C090)
	  line(cx - 10, cy + 4, cx + 10, cy - 2, 0x8A7450)
	end
  end

  def draw_ship()
	var x = 8 + int(math.sin(self.tp * 0.2) * 5)
	var y = 12 + int(math.sin(self.tp * 0.7) * 1.2)
	var len = self.warp > 0 ? 6 : 2 + (self.tk % 3 == 0 ? 1 : 0)
	line(x - 1 - len, y, x - 1, y, self.warp > 0 ? 0x80D0FF : 0xFF9020)
	rect_fill(x, y, 5, 1, 0xC0C8D8)
	pixel(x + 5, y, 0xFFFFFF)
	pixel(x + 3, y - 1, 0x40C0FF)
	pixel(x, y - 1, 0x808898)
	pixel(x, y + 1, 0x808898)
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 33
	  self.last = t
	  self.step()
	end
	self.tp = t / 1000.0
	clear(rgb(2, 3, 12))
	self.draw_stars()
	self.draw_planet()
	self.draw_ship()
  end
end

return App()
