# name: Block Drop
# summary: Falling blocks on a tall little well. Knob moves, + rotates, - drops it faster, press slams it down.
# author: Stipple
# tags: game, puzzle, classic, interactive
# panel: 52x16

# @input exclusive

import math
import string

# The well is 10 wide and the full 16 tall, standing at the right of the
# panel with the score and next piece beside it. A piece is four [x, y]
# offsets; turning one is (x, y) -> (-y, x), so there is no table of
# rotations to get wrong.
class App
  var board, pal, kinds
  var cur, px, py, kind
  var nextKind
  var score, lines, best
  var dead, last, lastFall

  def init()
	self.best = 0
	self.pal = [rgb(80, 230, 255), rgb(255, 230, 80), rgb(190, 100, 255), rgb(100, 240, 120),
				rgb(255, 90, 90), rgb(90, 130, 255), rgb(255, 160, 60)]
	self.kinds = [
	  [[0,0],[1,0],[2,0],[3,0]],
	  [[0,0],[1,0],[0,1],[1,1]],
	  [[0,0],[1,0],[2,0],[1,1]],
	  [[1,0],[2,0],[0,1],[1,1]],
	  [[0,0],[1,0],[1,1],[2,1]],
	  [[0,0],[0,1],[1,1],[2,1]],
	  [[2,0],[0,1],[1,1],[2,1]]
	]
	self._reset()
  end

  def _reset()
	self.board = []
	for i : 0 .. 159
	  self.board.push(0)
	end
	self.score = 0
	self.lines = 0
	self.dead = false
	self.last = nil
	self.lastFall = nil
	self.nextKind = math.rand() % 7
	self._spawn()
  end

  def _spawn()
	self.kind = self.nextKind
	self.nextKind = math.rand() % 7
	self.cur = []
	for c : self.kinds[self.kind]
	  self.cur.push([c[0], c[1]])
	end
	self.px = 3
	self.py = 0
	if !self._fits(self.cur, self.px, self.py)
	  self.dead = true
	  if self.score > self.best self.best = self.score end
	  tone(130, 300)
	end
  end

  def _fits(cells, ox, oy)
	for c : cells
	  var x = ox + c[0]
	  var y = oy + c[1]
	  if x < 0 || x >= 10 || y >= 16 return false end
	  if y >= 0 && self.board[y * 10 + x] != 0 return false end
	end
	return true
  end

  def _level()
	return self.lines / 10
  end

  def _lock()
	for c : self.cur
	  var y = self.py + c[1]
	  if y >= 0
		self.board[y * 10 + self.px + c[0]] = self.kind + 1
	  end
	end
	var cleared = 0
	var y = 15
	while y >= 0
	  var full = true
	  for x : 0 .. 9
		if self.board[y * 10 + x] == 0 full = false end
	  end
	  if full
		cleared += 1
		var r = y
		while r > 0
		  for x : 0 .. 9
			self.board[r * 10 + x] = self.board[(r - 1) * 10 + x]
		  end
		  r -= 1
		end
		for x : 0 .. 9
		  self.board[x] = 0
		end
	  else
		y -= 1
	  end
	end
	if cleared > 0
	  self.lines += cleared
	  self.score += 100 * cleared * cleared * (1 + self._level())
	  tone(600 + cleared * 150, 80)
	end
	self._spawn()
  end

  def _fall()
	if self._fits(self.cur, self.px, self.py + 1)
	  self.py += 1
	  return true
	end
	self._lock()
	return false
  end

  def _turn()
	var t = []
	for c : self.cur
	  t.push([-c[1], c[0]])
	end
	# Nudge sideways if the turn bumps a wall - a kick, of the simplest kind.
	for k : [0, -1, 1, -2, 2]
	  if self._fits(t, self.px + k, self.py)
		self.cur = t
		self.px += k
		return
	  end
	end
  end

  def on_button(name)
	if self.dead
	  if name == 'select' self._reset() end
	  return
	end
	if name == 'left'
	  if self._fits(self.cur, self.px - 1, self.py) self.px -= 1 end
	elif name == 'right'
	  if self._fits(self.cur, self.px + 1, self.py) self.px += 1 end
	elif name == 'plus'
	  self._turn()
	elif name == 'minus'
	  self._fall()
	  self.score += 1
	elif name == 'select'
	  var n = 0
	  while self._fits(self.cur, self.px, self.py + 1)
		self.py += 1
		n += 1
	  end
	  self.score += n * 2
	  self._lock()
	end
  end

  def draw()
	var now = now_ms()
	if self.lastFall == nil self.lastFall = now end
	var interval = 600 - self._level() * 50
	if interval < 120 interval = 120 end
	if !self.dead && now - self.lastFall >= interval
	  self.lastFall = now
	  self._fall()
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

	var bx = 41
	rect_fill(bx - 1, 0, 1, 16, rgb(50, 50, 80))
	rect_fill(bx + 10, 0, 1, 16, rgb(50, 50, 80))
	rect_fill(bx, 0, 10, 16, rgb(8, 8, 16))

	for y : 0 .. 15
	  for x : 0 .. 9
		var v = self.board[y * 10 + x]
		if v != 0 pixel(bx + x, y, self.pal[v - 1]) end
	  end
	end

	# Ghost, then the piece itself.
	var gy = self.py
	while self._fits(self.cur, self.px, gy + 1)
	  gy += 1
	end
	for c : self.cur
	  pixel(bx + self.px + c[0], gy + c[1], rgb(40, 40, 60))
	end
	for c : self.cur
	  if self.py + c[1] >= 0
		pixel(bx + self.px + c[0], self.py + c[1], self.pal[self.kind])
	  end
	end

	text(2, 1, string.format('%d', self.score), rgb(255, 220, 160))
	text(2, 9, string.format('L%d', self.lines), rgb(120, 200, 255))

	text(28, 1, 'N', rgb(70, 70, 100))
	for c : self.kinds[self.nextKind]
	  pixel(33 + c[0], 2 + c[1], self.pal[self.nextKind])
	end
  end
end

return App()
