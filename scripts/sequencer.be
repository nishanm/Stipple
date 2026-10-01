# name: Sequencer
# summary: Sixteen steps of pentatonic melody that mutate as they loop, played and drawn at once.
# author: Stipple
# tags: audio, music, generative, animation
# panel: 52x16

import math

class App
  var NOTES        # Hz, low to high
  var TINT         # one colour per row
  var steps        # 16 entries: 0..4, or -1 for a rest
  var at           # step the playhead is on
  var last         # when that step began, on the device clock
  var loops

  def init()
    # A, C, D, E, G - pentatonic minor, one octave up so a small speaker
    # can actually produce them.
    self.NOTES = [440, 523, 587, 659, 784]
    self.TINT = [rgb(0, 160, 255), rgb(0, 200, 190), rgb(90, 210, 70),
                 rgb(240, 170, 40), rgb(240, 70, 140)]
    self.at = 0
    self.last = 0
    self.loops = 0
    self._reroll()
  end

  def _reroll()
    self.steps = []
    var i = 0
    while i < 16
      self.steps.push(self._note(i))
      i += 1
    end
  end

  # One step. Rests land on the off-beats, so the bar keeps a pulse
  # instead of dissolving.
  def _note(i)
    if i % 2 == 1 && (math.rand() % 3) == 0
      return -1
    end
    return math.rand() % 5
  end

  def on_button(name)
    self._reroll()
    self.at = 0
    self.loops = 0
  end

  def draw()
    clear(rgb(0, 0, 0))
    var now = now_ms()

    var struck = false
    if now - self.last >= 125
      self.last = now
      self.at += 1
      struck = true
      if self.at >= 16
        self.at = 0
        self.loops += 1
        # Two steps rewritten a bar.
        self.steps[math.rand() % 16] = self._note(math.rand() % 16)
        self.steps[math.rand() % 16] = self._note(math.rand() % 16)
      end
    end

    var note = self.steps[self.at]
    if struck && note >= 0 && audio_known()
      tone(self.NOTES[note], 110)
    end

    # The playhead first, as a dim column, so the notes drawn over it stay
    # the brightest thing on the panel.
    rect_fill(2 + self.at * 3, 1, 3, 15, rgb(16, 16, 22))

    var i = 0
    while i < 16
      var n = self.steps[i]
      if n >= 0
        var x = 2 + i * 3
        # Row 0 is the top of the panel and the highest note, which is the
        # way round anybody who has seen a piano roll expects.
        var y = 1 + (4 - n) * 3
        if i == self.at
          rect_fill(x, y, 3, 3, rgb(255, 255, 255))
        else
          rect_fill(x, y, 2, 2, self.TINT[n])
        end
      end
      i += 1
    end

    # A ruler on the free top row: four beats to the bar, so the eye can
    # hear the phrasing even when the device cannot play it.
    var b = 0
    while b < 4
      pixel(2 + b * 12, 0, rgb(60, 60, 70))
      b += 1
    end

    if !audio_known()
      # No room for a word among sixteen columns, so it goes in the corner
      # the grid does not use - and it is red, which nothing else here is.
      pixel(50, 0, rgb(160, 40, 40))
      pixel(51, 0, rgb(160, 40, 40))
      pixel(50, 1, rgb(160, 40, 40))
      pixel(51, 1, rgb(160, 40, 40))
    end
  end
end

return App()
