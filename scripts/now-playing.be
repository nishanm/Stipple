# name: Now Playing
# summary: Whatever is on the speakers, from MQTT - title, artist, and a bar that runs out with the track.
# author: Stipple
# tags: mqtt, music, home
# panel: 52x16

# @config title text "Title topic" default="home/media/title" maxlen=100 help="Save the script again after changing any topic."
# @config artist text "Artist topic" default="home/media/artist" maxlen=100
# @config state text "State topic" default="home/media/state" maxlen=100 help="Expects playing, paused or idle."
# @config pos text "Position topic" default="home/media/position" maxlen=100 help="Seconds into the track. Leave as it is if you do not publish one."
# @config dur text "Duration topic" default="home/media/duration" maxlen=100

class App
  var T, A, S, P, D
  var ok

  def init()
    self.T = store.get("title", "home/media/title")
    self.A = store.get("artist", "home/media/artist")
    self.S = store.get("state", "home/media/state")
    self.P = store.get("pos", "home/media/position")
    self.D = store.get("dur", "home/media/duration")
    self.ok = true
  end

  def duration()
    return 15000
  end

  def _num(topic)
    var raw = mqtt_get(topic)
    if raw == nil
      return nil
    end
    return int(real(raw))
  end

  # Left aligned when it fits, scrolling when it does not. A title is whatever
  # length somebody's music happens to be, so this is the one place on the
  # panel where scrolling earns its keep.
  def _line(s, y, colour, phase)
    var w = text_width(s)
    if w <= width()
      text(0, y, s, colour)
      return
    end
    # A gap of a full panel between repeats, so the end and the beginning are
    # never on screen together looking like one word.
    var span = w + width()
    var off = int((now_ms() / 45 + phase) % span)
    text(width() - off, y, s, colour)
    if width() - off + span < width()
      text(width() - off + span, y, s, colour)
    end
  end

  def _glyph(playing, c)
    if playing
      # A triangle, drawn as three shortening columns.
      line(47, 9, 47, 13, c)
      line(48, 10, 48, 12, c)
      pixel(49, 11, c)
      return
    end
    line(47, 9, 47, 13, c)
    line(49, 9, 49, 13, c)
  end

  def draw()
    clear(rgb(0, 0, 0))

    mqtt_watch(self.T)
    mqtt_watch(self.A)
    mqtt_watch(self.S)
    mqtt_watch(self.P)
    self.ok = mqtt_watch(self.D)

    if !mqtt_known()
      text(2, 0, "no", rgb(120, 120, 120))
      text(2, 9, "broker", rgb(120, 60, 60))
      return
    end

    if !self.ok
      text(0, 0, "re-save", rgb(230, 150, 60))
      text(0, 9, "to apply", rgb(120, 90, 50))
      return
    end

    var title = mqtt_get(self.T)
    var state = mqtt_get(self.S)

    if title == nil || title == ""
      text(1, 0, "nothing", rgb(70, 76, 90))
      text(1, 9, "playing", rgb(50, 54, 66))
      return
    end

    var playing = state == nil || state == "playing"

    var titleColour = rgb(235, 240, 246)
    var artistColour = rgb(120, 130, 150)
    if !playing
      # Paused is a real state and worth showing as one: the same screen
      # dimmed, rather than a different screen.
      titleColour = rgb(130, 135, 145)
      artistColour = rgb(80, 84, 95)
    end

    self._line(title, 0, titleColour, 0)

    var artist = mqtt_get(self.A)
    if artist != nil && artist != ""
      self._line(artist, 8, artistColour, 400)
    end

    self._glyph(playing, playing ? rgb(80, 200, 120) : rgb(120, 120, 130))

    # The progress bar only appears when both halves of it are known.
    # Guessing a duration would draw a bar that runs out at the wrong time,
    # which is worse than no bar at all.
    var at = self._num(self.P)
    var len = self._num(self.D)
    if at != nil && len != nil && len > 0
      if at > len
        at = len
      end
      var w = int(at * width() / len)
      rect_fill(0, 15, width(), 1, rgb(20, 22, 28))
      if w > 0
        rect_fill(0, 15, w, 1, rgb(70, 150, 220))
      end
    end
  end
end

return App()
