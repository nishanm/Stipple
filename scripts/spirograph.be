# name: Spirograph
# summary: A hypotrochoid draws itself in a slowly changing colour, leaving a fading trail.
# author: Stipple
# tags: animation, geometry, hypnotic
# panel: 52x16

import math

class App
  var a
  var xs
  var ys
  var last
  var hue

  def init()
	self.a = 0.0
	self.xs = []
	self.ys = []
	self.hue = 0
	self.last = now_ms()
  end

  def wheel(p, k)
	var a = p * 0.0245
	return rgb(int(k * (128 + 127 * math.sin(a))), int(k * (128 + 127 * math.sin(a + 2.094))), int(k * (128 + 127 * math.sin(a + 4.188))))
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 25
	  self.last = t
	  self.a += 0.11
	  self.hue += 2
	  # Outer radius 5, inner 3, pen 5 - gives a five-lobed rosette.
	  var x = 2.0 * math.cos(self.a) + 5.0 * math.cos(2.0 * self.a / 3.0)
	  var y = 2.0 * math.sin(self.a) - 5.0 * math.sin(2.0 * self.a / 3.0)
	  self.xs.push(26 + int(x * 3.6))
	  self.ys.push(8 + int(y * 1.05))
	  if self.xs.size() > 110
		self.xs.pop(0)
		self.ys.pop(0)
	  end
	end

	clear(rgb(0, 0, 0))
	var n = self.xs.size()
	for i : 0 .. n - 1
	  pixel(self.xs[i], self.ys[i], self.wheel(self.hue + i * 3, (i + 1) * 1.0 / n))
	end
  end
end

return App()
