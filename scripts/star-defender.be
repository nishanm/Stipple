# name: Star Defender
# summary: Hold the line against a descending swarm. The knob or -/+ moves your ship, press to fire. Each wave arrives faster.
# author: Stipple
# tags: game, arcade, interactive
# panel: 52x16

# @input exclusive

import math
import string

# The swarm is a grid that moves as one: it shuffles sideways, and when its
# edge reaches the wall it drops a row and turns. Fewer invaders left means it
# moves more often, which is the whole trick of the original.
class App
  var alive, ox, oy, dir
  var ship, shots, bombs
  var score, lives, wave, best
  var dead, last, lastMove, cool, flash

  def init()
	self.best = 0
	self._reset()
  end

  def _reset()
	self.score = 0
	self.lives = 3
	self.wave = 1
	self.dead = false
	self._swarm()
  end

  def _swarm()
	self.alive = []
	for i : 0 .. 23
	  self.alive.push(true)
	end
	self.ox = 2
	self.oy = 1 + (self.wave > 3 ? 2 : self.wave - 1)
	self.dir = 1
	self.ship = 24
	self.shots = []
	self.bombs = []
	self.cool = 0
	self.flash = 0
	self.last = nil
	self.lastMove = 0
  end

  def _count()
	var n = 0
	for a : self.alive
	  if a n += 1 end
	end
	return n
  end

  def _fire()
	if self.cool <= 0 && self.shots.size() < 2
	  self.shots.push([self.ship, 13])
	  self.cool = 5
	  tone(900, 25)
	end
  end

  def on_button(name)
	if self.dead
	  if name == 'select' self._reset() end
	  return
	end
	if name == 'left'
	  self.ship -= 3
	elif name == 'right'
	  self.ship += 3
	elif name == 'minus'
	  self.ship -= 2
	elif name == 'plus'
	  self.ship += 2
	elif name == 'select'
	  self._fire()
	end
	if self.ship < 1 self.ship = 1 end
	if self.ship > width() - 2 self.ship = width() - 2 end
  end

  def _moveSwarm()
	var edge = false
	for r : 0 .. 2
	  for c : 0 .. 7
		if self.alive[r * 8 + c]
		  var x = self.ox + c * 5 + self.dir * 1
		  if x < 0 || x > width() - 3 edge = true end
		end
	  end
	end
	if edge
	  self.dir = -self.dir
	  self.oy += 1
	else
	  self.ox += self.dir
	end
	if math.rand() % 3 == 0
	  var c = math.rand() % 8
	  var r = 2
	  while r >= 0
		if self.alive[r * 8 + c]
		  self.bombs.push([self.ox + c * 5 + 1, self.oy + r * 3 + 2])
		  r = -1
		else
		  r -= 1
		end
	  end
	end
  end

  def _lose()
	self.lives -= 1
	self.flash = 12
	tone(120, 250)
	self.bombs = []
	if self.lives <= 0
	  self.dead = true
	  if self.score > self.best self.best = self.score end
	end
  end

  def _tick()
	if self.cool > 0 self.cool -= 1 end
	if self.flash > 0 self.flash -= 1 end

	var keep = []
	for s : self.shots
	  s[1] -= 1
	  var hit = false
	  var c = (s[0] - self.ox) / 5
	  var r = (s[1] - self.oy) / 3
	  if s[0] >= self.ox && (s[0] - self.ox) % 5 < 3 && (s[1] - self.oy) % 3 < 2 && c >= 0 && c < 8 && r >= 0 && r < 3
		if self.alive[r * 8 + c]
		  self.alive[r * 8 + c] = false
		  self.score += 10 * (3 - r)
		  hit = true
		  tone(300, 30)
		end
	  end
	  if !hit && s[1] >= 0 keep.push(s) end
	end
	self.shots = keep

	var fall = []
	for b : self.bombs
	  b[1] += 1
	  if b[1] >= 15 && math.abs(b[0] - self.ship) <= 1
		self._lose()
		return
	  elif b[1] < 16
		fall.push(b)
	  end
	end
	self.bombs = fall

	var n = self._count()
	if n == 0
	  self.wave += 1
	  self.score += 50
	  self._swarm()
	  return
	end

	self.lastMove += 1
	var every = 2 + n / 3 - self.wave / 2
	if every < 2 every = 2 end
	if self.lastMove >= every
	  self.lastMove = 0
	  self._moveSwarm()
	end

	# Reached the bottom row of the field.
	if self.oy + 8 >= 15 && n > 0
	  var lowest = -1
	  for r : 0 .. 2
		for c : 0 .. 7
		  if self.alive[r * 8 + c] lowest = r end
		end
	  end
	  if self.oy + lowest * 3 + 2 >= 14
		self.lives = 1
		self._lose()
	  end
	end
  end

  def draw()
	var w = width()
	var now = now_ms()
	if self.last == nil self.last = now end
	if now - self.last >= 70
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

	# A few drifting stars, so the dark has some depth.
	for i : 0 .. 9
	  var sx = (i * 17 + now / 200) % w
	  pixel(sx, (i * 7) % 15, rgb(30, 30, 50))
	end

	for r : 0 .. 2
	  for c : 0 .. 7
		if self.alive[r * 8 + c]
		  var x = self.ox + c * 5
		  var y = self.oy + r * 3
		  var col = r == 0 ? rgb(255, 90, 200) : (r == 1 ? rgb(90, 220, 255) : rgb(120, 255, 130))
		  rect_fill(x, y, 3, 1, col)
		  pixel(x, y + 1, col)
		  pixel(x + 2, y + 1, col)
		  if (now / 400) % 2 == 0 pixel(x + 1, y + 1, col) end
		end
	  end
	end

	for s : self.shots
	  pixel(s[0], s[1], rgb(255, 255, 255))
	  pixel(s[0], s[1] + 1, rgb(120, 180, 255))
	end
	for b : self.bombs
	  pixel(b[0], b[1], rgb(255, 80, 60))
	end

	var body = self.flash > 0 && self.flash % 2 == 0 ? rgb(255, 60, 60) : rgb(255, 230, 90)
	rect_fill(self.ship - 1, 15, 3, 1, body)
	pixel(self.ship, 14, body)

	for l : 1 .. self.lives
	  pixel(w - l * 2, 0, rgb(255, 70, 90))
	end
  end
end

return App()
