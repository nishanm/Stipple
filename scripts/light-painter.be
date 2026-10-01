# name: Light Painter
# summary: Paint with light. The knob steers across, + and - steer up and down, and every stroke fades into a glowing afterimage.
# author: Stipple
# tags: interactive, generative, colour
# panel: 52x16

# @input exclusive

import math

# The knob moves the brush along, - and + move it down and up, and the press
# wipes the canvas. The brush wraps at the edges so there is never a wall to
# get stuck on. Middle button leaves, as it does everywhere.
class App
  var heat, hue
  var cx, cy
  var colour

  def init()
	self.heat = []
	self.hue = []
	var n = width() * height()
	for i : 0 .. n - 1
	  self.heat.push(0.0)
	  self.hue.push(0)
	end
	self.cx = width() / 2
	self.cy = height() / 2
	self.colour = 0
	self._stamp()
  end

  def _wheel(pos, k)
	pos = pos % 256
	var r = 0
	var g = 0
	var b = 0
	if pos < 85
	  r = 255 - pos * 3
	  g = pos * 3
	elif pos < 170
	  pos -= 85
	  g = 255 - pos * 3
	  b = pos * 3
	else
	  pos -= 170
	  r = pos * 3
	  b = 255 - pos * 3
	end
	return rgb(int(r * k), int(g * k), int(b * k))
  end

  def _light(x, y, amount)
	var w = width()
	var h = height()
	x = (x + w) % w
	y = (y + h) % h
	var i = y * w + x
	if self.heat[i] < amount
	  self.heat[i] = amount
	end
	self.hue[i] = self.colour
  end

  # A bright core with a dimmer halo, which is what makes a single pixel
  # read as light rather than as a dot.
  def _stamp()
	self._light(self.cx, self.cy, 1.0)
	self._light(self.cx - 1, self.cy, 0.45)
	self._light(self.cx + 1, self.cy, 0.45)
	self._light(self.cx, self.cy - 1, 0.45)
	self._light(self.cx, self.cy + 1, 0.45)
  end

  def _move(dx, dy)
	self.cx = (self.cx + dx + width()) % width()
	self.cy = (self.cy + dy + height()) % height()
	self.colour = (self.colour + 4) % 256
	self._stamp()
  end

  def on_button(name)
	if name == 'left'
	  self._move(-1, 0)
	elif name == 'right'
	  self._move(1, 0)
	elif name == 'plus'
	  self._move(0, -1)
	elif name == 'minus'
	  self._move(0, 1)
	elif name == 'select'
	  for i : 0 .. self.heat.size() - 1
		self.heat[i] = 0.0
	  end
	end
  end

  def draw()
	var w = width()
	var h = height()
	for y : 0 .. h - 1
	  for x : 0 .. w - 1
		var i = y * w + x
		var v = self.heat[i]
		if v > 0.03
		  # Squared, so the tail dies away softly instead of linearly.
		  pixel(x, y, self._wheel(self.hue[i], v * v))
		  self.heat[i] = v * 0.95
		else
		  self.heat[i] = 0.0
		end
	  end
	end

	# The brush itself, blinking so it can be found on a busy canvas.
	if (now_ms() / 250) % 2 == 0
	  pixel(self.cx, self.cy, rgb(255, 255, 255))
	end
  end
end

return App()
