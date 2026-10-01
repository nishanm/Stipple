# name: Magic 8 Ball
# summary: Press the button, the ball shakes, and it tells you what it thinks. Ask a yes or no question first.
# author: Stipple
# tags: game, interactive, fun
# panel: 52x16

import math

class App
  var state
  var count
  var pick
  var last
  var answers

  def init()
	self.state = 0
	self.count = 0
	self.pick = 0
	self.last = now_ms()
	# Two lines each, eight characters at most - that is what a line holds.
	self.answers = [
	  ["IT IS", "CERTAIN"], ["YES", "SURE"], ["SIGNS", "SAY YES"],
	  ["ASK", "LATER"], ["CANNOT", "PREDICT"], ["FOCUS &", "RETRY"],
	  ["DONT", "COUNT ON"], ["VERY", "DOUBTFUL"], ["MY REPLY", "IS NO"],
	  ["OUTLOOK", "GOOD"]
	]
  end

  def on_button(name)
	if name != "select"
	  return
	end
	self.state = 1
	self.count = 24
	self.pick = math.rand() % self.answers.size()
  end

  def draw()
	var t = now_ms()
	if t - self.last >= 50
	  self.last = t
	  if self.count > 0
		self.count -= 1
		if self.count == 0
		  self.state = self.state == 1 ? 2 : 0
		  self.count = self.state == 2 ? 100 : 0
		end
	  end
	end

	clear(rgb(0, 0, 0))
	var jitter = self.state == 1 ? (math.rand() % 5) - 2 : 0
	var bx = 18 + jitter
	# The ball: a dark disc with a shine.
	rect_fill(bx + 3, 0, 12, 16, rgb(20, 10, 40))
	rect_fill(bx + 1, 2, 16, 12, rgb(20, 10, 40))
	rect_fill(bx, 4, 18, 8, rgb(20, 10, 40))
	pixel(bx + 4, 3, rgb(110, 100, 150))
	pixel(bx + 5, 2, rgb(110, 100, 150))

	if self.state == 2
	  var a = self.answers[self.pick]
	  text(26 - text_width(a[0]) / 2, 2, a[0], rgb(90, 160, 255))
	  text(26 - text_width(a[1]) / 2, 9, a[1], rgb(90, 160, 255))
	else
	  text(bx + 6, 5, "8", rgb(240, 240, 240))
	end
  end
end

return App()
