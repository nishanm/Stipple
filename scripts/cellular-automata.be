# name: Cellular Automata
# summary: One-dimensional automata grow downwards from a single cell - Rule 30, 90, 110 and 150. Press to change rule.
# author: Stipple
# tags: animation, maths, generative, interactive
# panel: 52x16

import string

class App
  var rows
  var row
  var rules
  var which
  var gen
  var last

  def init()
	self.rules = [30, 90, 110, 150]
	self.which = 0
	self.last = now_ms()
	self.restart()
  end

  def restart()
	self.rows = []
	self.row = []
	for i : 0 .. width() - 1
	  self.row.push(i == width() / 2 ? 1 : 0)
	end
	self.rows.push(self.row)
	self.gen = 0
  end

  def next()
	var rule = self.rules[self.which]
	var w = width()
	var nr = []
	for i : 0 .. w - 1
	  var l = self.row[(i + w - 1) % w]
	  var c = self.row[i]
	  var r = self.row[(i + 1) % w]
	  var idx = l * 4 + c * 2 + r
	  nr.push((rule >> idx) & 1)
	end
	self.row = nr
	self.rows.push(nr)
	if self.rows.size() > height()
	  self.rows.pop(0)
	end
	self.gen += 1
	# Rule 90 and friends draw the same triangle forever - move on.
	if self.gen > 90
	  self.which = (self.which + 1) % self.rules.size()
	  self.restart()
	end
  end

  def on_button(name)
	if name != "select"
	  return
	end
	self.which = (self.which + 1) % self.rules.size()
	self.restart()
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 110
	  self.last = t
	  self.next()
	end
	clear(rgb(0, 0, 0))
	var n = self.rows.size()
	for y : 0 .. n - 1
	  var r = self.rows[y]
	  var b = 90 + (y * 165) / height()
	  for x : 0 .. width() - 1
		if r[x] == 1
		  pixel(x, y, rgb(b / 3, b, 255 - b / 2))
		end
	  end
	end
	text(width() - 19, 0, string.format("%d", self.rules[self.which]), rgb(255, 200, 60))
  end
end

return App()
