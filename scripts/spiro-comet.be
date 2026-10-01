# name: Spiro Comet
# summary: A comet drawing Lissajous curves. The knob changes its horizontal beat, - and + change the vertical one, press to shift colour.
# author: Stipple
# tags: interactive, generative, colour
# panel: 52x16

# @input exclusive

import math
import string

# The comet's
# tidy knots; changing one while it flies gives a moment of chaos before it
# settles into the next. The tail is the last stretch of the path, dimming.
class App
  var a, b
  var t
  var trail
  var shift

  def init()
	self.a = 3
	self.b = 2
	self.t = 0.0
	self.trail = []
	self.shift = 0
  end

  def _wheel(pos, k)
	pos = pos % 256
	var r = 0
	var g = 0
	var bl = 0
	if pos < 85
	  r = 255 - pos * 3
	  g = pos * 3
	elif pos < 170
	  pos -= 85
	  g = 255 - pos * 3
	  bl = pos * 3
	else
	  pos -= 170
	  r = pos * 3
	  bl = 255 - pos * 3
	end
	return rgb(int(r * k), int(g * k), int(bl * k))
  end

  def on_button(name)
	if name == 'left'
	  self.a -= 1
	  if self.a < 1 self.a = 1 end
	elif name == 'right'
	  self.a += 1
	  if self.a > 9 self.a = 9 end
	elif name == 'plus'
	  self.b += 1
	  if self.b > 9 self.b = 9 end
	elif name == 'minus'
	  self.b -= 1
	  if self.b < 1 self.b = 1 end
	elif name == 'select'
	  self.shift = (self.shift + 43) % 256
	end
  end

  def draw()
	var w = width()
	var h = height()

	# Three sub-steps a frame, so the head moves quickly without leaving gaps.
	for i : 0 .. 2
	  self.t += 0.035
	  var x = int((w - 1) / 2.0 + math.sin(self.a * self.t + 1.5708) * (w - 2) / 2.0 + 0.5)
	  var y = int((h - 1) / 2.0 + math.sin(self.b * self.t) * (h - 1) / 2.0 + 0.5)
	  self.trail.push([x, y])
	end
	while self.trail.size() > 110
	  self.trail.remove(0)
	end

	var n = self.trail.size()
	for i : 0 .. n - 1
	  var p = self.trail[i]
	  var k = real(i) / n
	  pixel(p[0], p[1], self._wheel(self.shift + int(k * 90), k * k))
	end

	if n > 0
	  var head = self.trail[n - 1]
	  pixel(head[0], head[1], rgb(255, 255, 255))
	end

	# The current ratio, so the knob's effect can be read as well as seen.
	text(0, 0, string.format('%d:%d', self.a, self.b), rgb(40, 40, 50))
  end
end

return App()
