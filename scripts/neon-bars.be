# name: Neon Bars
# summary: The room's volume as a scrolling wall of neon, with peak hold and its own auto-gain.
# author: Stipple
# tags: audio, microphone, animation, ambient
# panel: 52x16

# A visualiser that shows what the device can actually hear.
#
# Not a spectrum analyser, and that is the one design decision here worth
# defending. The TC002's microphone reports a single amplitude about twenty
# times a second - one number, no bands. A script drawing eight coloured
# columns labelled 60Hz to 16kHz would be making seven of them up, and it
# would look convincing, which is what makes it the wrong thing to build.
#
# So every column is a real reading, and the axis across the panel is time
# rather than frequency. Fifty-two columns is about two and a half seconds of
# room, scrolling right to left - which turns out to read better than fake
# bands anyway, because a beat becomes a shape you can follow.
#
# Auto-gain lives here rather than in the adapter. What counts as loud depends
# on the room and no driver can know that; this one tracks its own recent
# maximum and scales to it, so it fills the panel in a quiet study and still
# has headroom at a party.
#
# Click to change palette.

class App
  var PAL           # three palettes, each low to high
  var pal
  var hist          # 52 amplitudes, newest last
  var peak          # 52 peak-hold heights
  var last          # when the last sample was taken
  var ceil          # current auto-gain ceiling
  var lit           # frames since anything was heard

  def init()

    self.PAL = [
      [rgb(0, 40, 90), rgb(0, 120, 200), rgb(0, 200, 255), rgb(150, 255, 255)],
      [rgb(60, 0, 70), rgb(160, 0, 140), rgb(255, 40, 160), rgb(255, 180, 230)],
      [rgb(0, 60, 20), rgb(20, 160, 40), rgb(120, 230, 40), rgb(230, 255, 140)]
    ]
    self.pal = store.get("pal", 0)
    if self.pal < 0 || self.pal >= size(self.PAL)
      self.pal = 0
    end

    self.hist = []
    self.peak = []
    var i = 0
    while i < 52
      self.hist.push(0)
      self.peak.push(0)
      i += 1
    end
    self.last = 0
    # A floor under the gain, so a silent room does not amplify its own noise
    # into a full-height wall of nothing.
    self.ceil = 2000
    self.lit = 0
  end

  def on_button(name)
    self.pal = (self.pal + 1) % size(self.PAL)
    store.set("pal", self.pal)
  end

  def draw()
    clear(rgb(0, 0, 0))

    if !mic_known()
      # A device that cannot hear and a silent room look identical on a
      # visualiser, and only one of them is worth fixing. ADR 0013.
      text(2, 1, "no mic", rgb(150, 60, 60))
      text(2, 9, "on this", rgb(70, 70, 70))
      return
    end

    var now = now_ms()

    if now - self.last >= 50
      self.last = now
      var v = mic_level()

      var i = 0
      while i < 51
        self.hist[i] = self.hist[i + 1]
        self.peak[i] = self.peak[i + 1]
        i += 1
      end
      self.hist[51] = v
      self.peak[51] = 0

      if v > 300
        self.lit = 0
      else
        self.lit += 1
      end
    end

    var top = 0
    var i = 0
    while i < 52
      if self.hist[i] > top
        top = self.hist[i]
      end
      i += 1
    end
    if top > self.ceil
      self.ceil = top
    elif self.ceil > 2000
      self.ceil = self.ceil - self.ceil / 64 - 1
    end

    var pal = self.PAL[self.pal]
    var h = 16

    rect_fill(0, 15, 52, 1, rgb(6, 10, 16))

    i = 0
    while i < 52
      var bar = (self.hist[i] * h) / self.ceil
      if bar > h
        bar = h
      end

      if bar > self.peak[i]
        self.peak[i] = bar
      elif self.peak[i] > 0 && (i % 3) == (now / 120) % 3
        self.peak[i] -= 1
      end

      var y = 0
      while y < bar
        # Four bands up the bar. Cheap - one comparison per pixel - and the
        # reason a 16-pixel column reads as intensity rather than as height.
        var shade = pal[0]
        var frac = (y * 4) / h
        if frac >= 3
          shade = pal[3]
        elif frac == 2
          shade = pal[2]
        elif frac == 1
          shade = pal[1]
        end
        pixel(i, 15 - y, shade)
        y += 1
      end

      if self.peak[i] > 0 && self.peak[i] <= h
        pixel(i, 16 - self.peak[i], rgb(255, 255, 255))
      end
      i += 1
    end

    # Ten seconds of nothing. Says which kind of nothing, because a
    # visualiser showing a flat line is the most ambiguous thing on a shelf.
    if self.lit > 200
      text(9, 5, "quiet", rgb(40, 40, 50))
    end
  end
end

return App()
