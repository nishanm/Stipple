# name: Sky Runner
# summary: Run a sunset ridge and leap the gaps and blocks. + or the knob press jumps, - drops you fast. Faster the further you go.
# author: Stipple
# tags: game, platformer, interactive, landscape
# panel: 52x16

# @input exclusive

import math
import string

# An endless runner over a parallax landscape: sunset sky, a low sun, two
# ranges of mountains drifting at different speeds, and a ridge of grass that
# has holes in it. Ground is stored as one number per column - the row its
# top is on - where a value of height() means there is no floor at all.
class App
  var cols
  var y, vy, grounded
  var dist, acc, speed
  var dead, last, best
  var gap, block, flat
  var stars

  def init()
	self.best = 0
	self.stars = []
	for i : 0 .. 13
	  self.stars.push([math.rand() % width(), math.rand() % 6, math.rand() % 3000])
	end
	self._reset()
  end

  def _floor()
	return height() - 3
  end

  def _reset()
	self.cols = []
	for i : 0 .. width() + 1
	  self.cols.push(self._floor())
	end
	self.y = self._floor() - 1.0
	self.vy = 0.0
	self.grounded = true
	self.dist = 0
	self.acc = 0.0
	self.speed = 0.5
	self.dead = false
	self.gap = 0
	self.block = 0
	self.flat = 14
	self.last = nil
  end

  def _nextColumn()
	var f = self._floor()
	if self.gap > 0
	  self.gap -= 1
	  self.flat = 8
	  return height()
	end
	if self.block > 0
	  self.block -= 1
	  self.flat = 8
	  return f - 2
	end
	if self.flat > 0
	  self.flat -= 1
	  return f
	end
	var roll = math.rand() % 3
	if roll == 0
	  self.gap = 3 + math.rand() % 2
	elif roll == 1
	  self.block = 2
	else
	  self.flat = 3
	end
	return f
  end

  def on_button(name)
	if self.dead
	  if name == 'select' self._reset() end
	  return
	end
	if name == 'plus' || name == 'select' || name == 'left'
	  if self.grounded
		self.vy = -1.7
		self.grounded = false
		tone(660, 40)
	  end
	elif name == 'minus' || name == 'right'
	  if !self.grounded self.vy += 1.2 end
	end
  end

  def _tick()
	var h = height()
	var px = 10
	self.acc += self.speed
	while self.acc >= 1.0
	  self.acc -= 1.0
	  self.cols.remove(0)
	  self.cols.push(self._nextColumn())
	  self.dist += 1
	end
	self.speed = 0.5 + self.dist / 900.0
	if self.speed > 1.0 self.speed = 1.0 end

	var oldY = self.y
	self.vy += 0.32
	self.y += self.vy
	var ground = self.cols[px]
	if ground < h && self.vy >= 0 && oldY <= ground - 1 && self.y >= ground - 1
	  self.y = ground - 1.0
	  self.vy = 0.0
	  self.grounded = true
	else
	  self.grounded = false
	end

	# Ran into the side of a block, or fell out of the world.
	if ground < h && ground <= int(self.y + 0.5) && !self.grounded
	  self._die()
	end
	if self.y > h + 2
	  self._die()
	end
  end

  def _die()
	self.dead = true
	if self.dist > self.best self.best = self.dist end
	tone(150, 250)
  end

  def _mountains(offset, base, amp, colour, period)
	var w = width()
	for x : 0 .. w - 1
	  var u = (x + offset) / period
	  var top = int(base - amp * (0.5 + 0.35 * math.sin(u) + 0.15 * math.sin(u * 2.3 + 1.0)))
	  if top < base
		rect_fill(x, top, 1, base - top + 1, colour)
	  end
	end
  end

  def draw()
	var w = width()
	var h = height()
	var now = now_ms()
	var f = self._floor()

	if self.last == nil self.last = now end
	if now - self.last >= 45
	  self.last = now
	  if !self.dead self._tick() end
	end

	# Sky, deep violet to hot orange at the horizon.
	for row : 0 .. f - 1
	  var k = real(row) / (f - 1)
	  rect_fill(0, row, w, 1, rgb(int(14 + 230 * k * k), int(6 + 90 * k * k), int(50 + 40 * k - 40 * k * k)))
	end

	for s : self.stars
	  if (now / 400 + s[2]) % 5 != 0
		pixel(s[0], s[1], rgb(120, 110, 150))
	  end
	end

	# The sun, half sunk.
	for dy : -3 .. 3
	  for dx : -5 .. 5
		if dx * dx + dy * dy * 2 <= 20
		  var sy = f - 3 + dy
		  if sy < f
			pixel(38 + dx, sy, rgb(255, 200 + dy * 8, 90 + dy * 12))
		  end
		end
	  end
	end

	self._mountains(self.dist / 6, f, 8, rgb(70, 30, 90), 5.0)
	self._mountains(self.dist / 3 + 40, f, 5, rgb(38, 16, 60), 3.5)

	# Ground.
	for x : 0 .. w - 1
	  var top = self.cols[x]
	  if top < h
		pixel(x, top, rgb(90, 200, 90))
		if top + 1 < h
		  rect_fill(x, top + 1, 1, h - top - 1, top < f ? rgb(120, 110, 130) : rgb(90, 60, 40))
		end
		if top < f
		  pixel(x, top, rgb(190, 180, 200))
		end
	  end
	end

	# The runner: two pixels tall, with a scarf that trails behind.
	var py = int(self.y + 0.5)
	var body = self.dead ? rgb(255, 60, 60) : rgb(255, 255, 255)
	pixel(10, py, body)
	pixel(10, py - 1, body)
	if !self.dead
	  pixel(9, py - 1, rgb(255, 80, 120))
	  pixel(8, py - 1 + (now / 120) % 2, rgb(200, 50, 90))
	end

	if self.dead
	  # The font is 5x7 and a line holds eight characters, so nothing here
	  # can be one line. Two columns instead: the verdict on the left, the
	  # numbers on the right, over a dimmed scene so it reads.
	  rect_fill(0, 0, w, h, rgb(0, 0, 0))
	  text(4, 1, 'GAME', rgb(255, 255, 255))
	  text(4, 9, 'OVER', rgb(255, 90, 90))
	  var score = string.format('%d', self.dist)
	  text(30, 1, score, rgb(255, 220, 160))
	  if (now / 1500) % 2 == 0
		text(30, 9, string.format('B%d', self.best), rgb(255, 200, 120))
	  else
		text(30, 9, 'PUSH', rgb(120, 200, 255))
	  end
	else
	  var score = string.format('%d', self.dist)
	  text(w - text_width(score), 0, score, rgb(255, 220, 160))
	end
  end
end

return App()
