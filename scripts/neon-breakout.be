# name: Neon Breakout
# summary: Break the glowing wall. The knob or -/+ slides the paddle, press to launch. Clear it and the next wall comes faster.
# author: Stipple
# tags: game, arcade, interactive
# panel: 52x16

# @input exclusive

import math
import string

# Bricks are a grid of 13 columns by 4 rows, each 4 pixels wide with a pixel
# of gap. The ball moves in fractions of a pixel, so a brick is found by
# rounding where it is rather than by stepping it cell to cell.
class App
  var bricks, left
  var bx, by, vx, vy
  var pad, lives, score, level, best
  var stuck, dead, last, flash
  var trail

  def init()
	self.best = 0
	self._reset()
  end

  def _reset()
	self.score = 0
	self.lives = 3
	self.level = 1
	self.dead = false
	self._wall()
  end

  def _wall()
	self.bricks = []
	for i : 0 .. 51
	  self.bricks.push(true)
	end
	self.left = 52
	self.pad = 22
	self.flash = 0
	self._park()
  end

  def _park()
	self.stuck = true
	self.trail = []
	self.bx = self.pad + 4.0
	self.by = 14.0
	self.vx = 0.0
	self.vy = 0.0
	self.last = nil
  end

  def _speed()
	return 0.5 + self.level * 0.08
  end

  def _slide(d)
	self.pad += d
	if self.pad < 0 self.pad = 0 end
	if self.pad > width() - 8 self.pad = width() - 8 end
  end

  def on_button(name)
	if self.dead
	  if name == 'select' self._reset() end
	  return
	end
	if name == 'left'
	  self._slide(-3)
	elif name == 'right'
	  self._slide(3)
	elif name == 'minus'
	  self._slide(-2)
	elif name == 'plus'
	  self._slide(2)
	elif name == 'select'
	  if self.stuck
		self.stuck = false
		self.vx = 0.3
		self.vy = -self._speed()
	  end
	end
  end

  def _brickAt(x, y)
	var r = y - 1
	var c = x / 4
	if r < 0 || r > 3 || c < 0 || c > 12 || x < 0 return -1 end
	var i = r * 13 + c
	return self.bricks[i] ? i : -1
  end

  def _tick()
	if self.stuck
	  self.bx = self.pad + 4.0
	  return
	end
	self.trail.push([self.bx, self.by])
	if self.trail.size() > 5 self.trail.remove(0) end

	self.bx += self.vx
	self.by += self.vy
	var w = width()
	if self.bx < 0
	  self.bx = 0.0
	  self.vx = -self.vx
	end
	if self.bx > w - 1
	  self.bx = w - 1.0
	  self.vx = -self.vx
	end
	if self.by < 0
	  self.by = 0.0
	  self.vy = -self.vy
	end

	var hit = self._brickAt(int(self.bx), int(self.by + 0.5))
	if hit >= 0
	  self.bricks[hit] = false
	  self.left -= 1
	  self.score += 10 * self.level
	  self.vy = -self.vy
	  tone(500 + (hit / 13) * 120, 30)
	  if self.left <= 0
		self.level += 1
		self._wall()
		return
	  end
	end

	# Paddle: where it lands decides the angle.
	if self.by >= 14 && self.vy > 0 && self.bx >= self.pad - 1 && self.bx <= self.pad + 8
	  self.by = 13.9
	  self.vy = -self.vy
	  self.vx = (self.bx - (self.pad + 3.5)) * 0.14
	  self.flash = 4
	end

	if self.by > 16
	  self.lives -= 1
	  tone(140, 200)
	  if self.lives <= 0
		self.dead = true
		if self.score > self.best self.best = self.score end
	  else
		self._park()
	  end
	end
  end

  def _brickColour(r)
	if r == 0 return rgb(255, 60, 110) end
	if r == 1 return rgb(255, 150, 40) end
	if r == 2 return rgb(80, 230, 120) end
	return rgb(70, 150, 255)
  end

  def draw()
	var w = width()
	var h = height()
	var now = now_ms()
	if self.last == nil self.last = now end
	if now - self.last >= 35
	  self.last = now
	  if !self.dead self._tick() end
	end

	if self.dead
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

	for r : 0 .. 3
	  for c : 0 .. 12
		if self.bricks[r * 13 + c]
		  rect_fill(c * 4, r + 1, 3, 1, self._brickColour(r))
		end
	  end
	end

	var n = self.trail.size()
	for i : 0 .. n - 1
	  var p = self.trail[i]
	  var k = int(20 + 160 * (i + 1) / (n + 1))
	  pixel(int(p[0]), int(p[1] + 0.5), rgb(k, k, k + 40 > 255 ? 255 : k + 40))
	end
	pixel(int(self.bx), int(self.by + 0.5), rgb(255, 255, 255))

	var pc = self.flash > 0 ? rgb(255, 255, 255) : rgb(120, 220, 255)
	if self.flash > 0 self.flash -= 1 end
	rect_fill(self.pad, h - 1, 8, 1, pc)

	for l : 1 .. self.lives
	  pixel(w - l * 2, 0, rgb(255, 70, 90))
	end
  end
end

return App()
