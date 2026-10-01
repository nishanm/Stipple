# name: Metronome
# summary: A swinging pendulum that keeps time out loud, at a tempo you click through.
# author: Stipple
# tags: audio, music, tool
# panel: 52x16

# The first thing worth doing with a speaker on a shelf clock.
#
# Two details make it a metronome rather than a picture of one. It runs off
# now_ms() - the device clock - not the frame counter, so the beat does not
# drift when the panel is busy and does not stop while the carousel is
# showing something else. And the beat is decided by which beat number the
# clock is in, never by counting frames since the last one: a frame the
# renderer skipped would otherwise swallow a beat.
#
# On a device with no speaker it still swings, and says MUTE so that the
# silence is the hardware's rather than something you might fix by turning
# it up.

class App
  var COS          # cosine * 1000, 32 steps round the full swing
  var TEMPOS
  var t            # index into TEMPOS
  var beat         # the beat number last sounded, from the device clock
  var flash        # when that beat landed

  def init()
    self.COS = [1000, 981, 924, 831, 707, 556, 383, 195, 0, -195, -383, -556,
                -707, -831, -924, -981, -1000, -981, -924, -831, -707, -556,
                -383, -195, 0, 195, 383, 556, 707, 831, 924, 981]
    self.TEMPOS = [60, 72, 84, 96, 108, 120, 132, 144, 160, 180, 200]
    self.t = store.get("tempo", 5)
    if self.t < 0 || self.t >= size(self.TEMPOS)
      self.t = 5
    end
    self.beat = -1
    self.flash = -10000
  end

  def on_button(name)
    self.t = (self.t + 1) % size(self.TEMPOS)
    store.set("tempo", self.t)
    # Re-anchor so the new tempo starts on a beat rather than wherever the
    # old one happened to be.
    self.beat = -1
  end

  def draw()
    clear(rgb(0, 0, 0))

    var bpm = self.TEMPOS[self.t]
    var period = 60000 / bpm
    var now = now_ms()

    # Which beat we are in, and how far through it. Both from the clock, so
    # a dropped frame costs a frame and not a beat.
    var n = now / period
    var into = now % period

    if n != self.beat
      # Skipping forward - after the carousel has been elsewhere for a
      # minute - must not replay the beats that went past unheard.
      self.beat = n
      self.flash = now
      if audio_known()
        if n % 4 == 0
          tone(1600, 30)
        else
          tone(1000, 22)
        end
      end
    end

    # The pendulum takes two beats to go out and back, so the swing is
    # indexed over 2 * period.
    var swing = (2 * period)
    var i = ((now % swing) * 32) / swing
    var dx = (-18 * self.COS[i]) / 1000
    var bx = 26 + dx
    # A shallow arc. A true pendulum of this width would swing off the top
    # of a panel only sixteen pixels tall.
    var by = 8 - (324 - dx * dx) / 100

    # Bright for the first 90 ms of the beat, then settled. Long enough to
    # catch at 200 bpm, short enough not to smear into the next one.
    var hot = (now - self.flash) < 90
    var rod = hot ? rgb(90, 70, 20) : rgb(45, 35, 10)
    var bob = hot ? rgb(255, 255, 255) : rgb(255, 176, 0)

    line(26, 0, bx, by, rod)
    rect_fill(bx - 1, by - 1, 3, 3, bob)

    var b = n % 4
    var k = 0
    while k < 4
      var x = 34 + k * 4
      if k < b
        rect_fill(x, 0, 3, 3, rgb(120, 60, 0))
      elif k == b
        var w = 1 + (into * 3) / period
        if w > 3 w = 3 end
        rect_fill(x, 0, w, 3, rgb(255, 176, 0))
      else
        pixel(x + 1, 1, rgb(50, 30, 0))
      end
      k += 1
    end

    text(1, 9, str(bpm), rgb(110, 110, 110))

    if !audio_known()
      text(27, 9, "MUTE", rgb(150, 40, 40))
    end
  end
end

return App()
