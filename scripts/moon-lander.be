# name: Moon Lander
# summary: Bring her down soft on the lit pad. + fires the engine, the knob nudges sideways. Fuel is short, and each landing narrows the pad.
# author: Stipple
# tags: game, physics, interactive, landscape
# panel: 52x16

# @input exclusive

import math
import string

# Gravity pulls a little every tick; each press of + pushes back a lot, and
# the knob gives a sideways nudge. A landing counts if you touch the pad
# slowly enough in both directions. Anything else is a crater.
class App
  var ground, padX, padW
  var x, y, vx, vy, fuel
  var level, score, best
  var state, last, burn, bits, stars

  def init()
	self.best = 0
	self.level = 1
	self.score = 0
	self.stars = []
	for i : 0 .. 15
	  self.stars.push([math.rand() % width(), math.rand() % 8, math.rand() % 5000])
	end
	self._start()
  end

  def _start()
	var w = width()
	self.padW = 10 - self.level
	if self.padW < 4 self.padW = 4 end
	self.padX = 6 + math.rand() % (w - 12 - self.padW)
	self.ground = []
	var h = 12.0
	for i : 0 .. w - 1
	  if i >= self.padX && i < self.padX + self.padW
		self.ground.push(13)
	  else
		h += (math.rand() % 3 - 1) * 0.6
		if h < 9.0 h = 9.0 end
		if h > 14.0 h = 14.0 end
		self.ground.push(int(h))
	  end
	end
	self.x = 4.0 + math.rand() % (w - 8)
	self.y = 1.0
	self.vx = 0.0
	self.vy = 0.0
	self.fuel = 24
	self.state = 'fly'
	self.last = nil
	self.burn = 0
	self.bits = []
  end

  def on_button(name)
	if self.state == 'dead'
	  if name == 'select'
		self.level = 1
		self.score = 0
		self._start()
	  end
	  return
	end
	if self.state == 'won'
	  if name == 'select'
		self.level += 1
		self._start()
	  end
	  return
	end
	if name == 'plus' || name == 'select'
	  if self.fuel > 0
		self.fuel -= 1
		self.vy -= 0.16
		self.burn = 4
		tone(220, 30)
	  end
	elif name == 'left'
	  if self.fuel > 0
		self.vx -= 0.08
		self.fuel -= 0.25
	  end
	elif name == 'right'
	  if self.fuel > 0
		self.vx += 0.08
		self.fuel -= 0.25
	  end
	end
  end

  def _crash()
	self.state = 'dead'
	if self.score > self.best self.best = self.score end
	tone(100, 350)
	for i : 0 .. 11
	  self.bits.push([self.x, self.y, (math.rand() % 100 - 50) / 60.0, -(math.rand() % 80) / 60.0])
	end
  end

  def _tick()
	if self.burn > 0 self.burn -= 1 end
	self.vy += 0.014
	self.x += self.vx
	self.y += self.vy
	var w = width()
	if self.x < 1
	  self.x = 1.0
	  self.vx = 0.0
	end
	if self.x > w - 2
	  self.x = w - 2.0
	  self.vx = 0.0
	end
	if self.y < 0
	  self.y = 0.0
	  self.vy = 0.0
	end
	var cx = int(self.x + 0.5)
	var low = self.ground[cx]
	for d : -1 .. 1
	  var g = self.ground[cx + d]
	  if g < low low = g end
	end
	if self.y >= low - 1
	  var onPad = cx - 1 >= self.padX && cx + 1 < self.padX + self.padW
	  if onPad && self.vy < 0.5 && math.abs(self.vx) < 0.25
		self.state = 'won'
		self.y = 12.0
		self.score += 100 * self.level + int(self.fuel) * 5
		if self.score > self.best self.best = self.score end
		tone(880, 120)
	  else
		self._crash()
	  end
	end
  end

  def draw()
	var w = width()
	var h = height()
	var now = now_ms()
	if self.last == nil self.last = now end
	if now - self.last >= 50
	  self.last = now
	  if self.state == 'fly' self._tick() end
	end

	if self.state == 'dead'
	  for b : self.bits
		b[0] += b[2] * 0.3
		b[1] += b[3] * 0.3
		b[3] += 0.03
	  end
	end

	for row : 0 .. h - 1
	  rect_fill(0, row, w, 1, rgb(4, 4, int(14 + row * 2)))
	end
	for s : self.stars
	  if (now / 500 + s[2]) % 4 != 0 pixel(s[0], s[1], rgb(110, 110, 150)) end
	end
	# A distant Earth, because a moon needs one.
	for dy : -2 .. 2
	  for dx : -2 .. 2
		if dx * dx + dy * dy <= 5 pixel(45 + dx, 3 + dy, rgb(60, 110 + dx * 10, 200)) end
	  end
	end

	for x : 0 .. w - 1
	  var g = self.ground[x]
	  rect_fill(x, g, 1, h - g, rgb(110, 105, 120))
	  pixel(x, g, rgb(190, 185, 200))
	end
	var lit = (now / 300) % 2 == 0 ? rgb(255, 230, 80) : rgb(80, 255, 120)
	rect_fill(self.padX, 13, self.padW, 1, lit)

	if self.state == 'dead'
	  for b : self.bits
		if b[1] < h pixel(int(b[0]), int(b[1]), rgb(255, 160 + int(b[3] * 20), 60)) end
	  end
	  text(4, 1, 'GAME', rgb(255, 255, 255))
	  text(4, 9, 'OVER', rgb(255, 90, 90))
	  text(30, 1, string.format('%d', self.score), rgb(255, 220, 160))
	  if (now / 1500) % 2 == 0
		text(30, 9, string.format('B%d', self.best), rgb(255, 200, 120))
	  else
		text(30, 9, 'PUSH', rgb(120, 200, 255))
	  end
	  return
	end

	var cx = int(self.x + 0.5)
	var cy = int(self.y + 0.5)
	pixel(cx, cy - 1, rgb(255, 255, 255))
	rect_fill(cx - 1, cy, 3, 1, rgb(200, 210, 230))
	if self.state == 'fly'
	  if self.burn > 0
		pixel(cx, cy + 1, rgb(255, 200, 60))
		pixel(cx, cy + 2, rgb(255, 90, 30))
	  end
	else
	  text(2, 1, 'SAFE', rgb(120, 255, 140))
	  text(30, 1, string.format('%d', self.score), rgb(255, 220, 160))
	end

	# Fuel gauge along the top edge.
	var f = int(self.fuel * w / 24)
	if f > 0 rect_fill(0, 0, f, 1, self.fuel < 6 ? rgb(255, 60, 60) : rgb(80, 200, 255)) end
  end
end

return App()
