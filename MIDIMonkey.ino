// MIDIMonkey. 
// Listens to MIDI message on one pin and triggers drums on the other 4 or 5 pins.
// Copyright (C) 2018, Aleh Dzenisiuk. All rights reserved.

// The code is for ATtiny85, but should work on any Arduino.
// (For ATtiny85 you can use "Digispark (8mhz - no USB)" board in the Arduino IDE.)

// TODO: make interrupt-driven MIDI reception
// TODO: use normal timer for the pulse handlers

#include <a21.hpp>

using namespace a21;

SerialTx<FastPin<4>, 115200> debugOut;
SerialRx<FastPin<0>, 31250> midi;

template <class T>
class MIDIParser {

protected:

  typedef MIDIParser<T> Self;

  static Self& getSelf() {
    static Self s = Self();
    return s;
  }

  // To avoid shifts event numbers are status bytes masked with 0x70.
  enum Event : uint8_t {
    EventNoteOff = 0x00,
    EventNoteOn = 0x10,
    EventPolyAftertouch = 0x20,
    EventControlChange = 0x30,
    EventProgramChange = 0x40,
    EventAftertouch = 0x50,
    EventPitchBend = 0x60,
    // To represent all the events we don't care about.
    EventUnknown = 0xF0
  } event;

  // You can find MIDI note as a remainder after division by 12.
  enum Note : uint8_t {
    NoteC = 0,
    NoteCs,
    NoteD,
    NoteDs,
    NoteE,
    NoteF,
    NoteFs,
    NoteG,
    NoteGs,
    NoteA,
    NoteAs,
    NoteB
  };

  // The number of arguments (parameters) expected for the given MIDI event. 
  // Note that we don't handle everything.
  static uint8_t argsForEvent(Event e) {
    return (EventNoteOff <= e && e <= EventControlChange || e == EventPitchBend) ? 2 : 1;
  }

  // The channel of the current MIDI event.
  uint8_t channel;

  // The arguments of the current MIDI event. 
  uint8_t args[2];

  // Number of valid arguments in args collected so far.
  uint8_t argsCollected;

  // Calls the handler method if we've got enough args for the current MIDI event.
  static void handleEventIfFinished() {
    Self& self = getSelf();
    if (self.event != EventUnknown && self.argsCollected == argsForEvent(self.event)) {
        T::handleEvent(self.event, self.channel, self.args);
        self.event = EventUnknown;
    }    
  }

  // This is eventually called by the parser, put your handler here.
  static void handleEvent(Event event, uint8_t channel, const uint8_t *args) {
  }
  
public:

  static void begin() {
    getSelf().event = EventUnknown;
  }
  
  static void handleByte(uint8_t b) {

    Self& self = getSelf();
    
    if (b & 0x80) {

      // A status byte always begins a new message.
      
      if (self.event != EventUnknown) {
        // Another command started before the previous one was fully read.
        // Something is wrong with our expectations or the stream is corrupted.
        // Can output some diagnostics or blink lights if it's important.
      }
      
      self.event = (Event)(b & 0x70);
      self.channel = b & 0x0F;

      // We simply skip those extra events.
      if (self.event >= EventPitchBend) {
        self.event = EventUnknown;
      }
      
      self.argsCollected = 0;
      
      // We don't currently have events without args, but still let's call to not forget later.
      handleEventIfFinished();
      
    } else {
      
      // Data bytes aka event arguments.
      
      if (self.event == EventUnknown) {
        // Skipping stray bytes or args of unknown events.
      } else {
        // Collecting event bytes.
        self.args[self.argsCollected++] = b;      
        handleEventIfFinished();
      }
    }
  }
};

// This is to store the state of a single trigger.
// Duration_us is the duration of the trig pulse itself.
template<class pin, uint32_t timer_period_us, uint16_t duration_us>
class PinPulser {
protected:

  typedef PinPulser<pin, timer_period_us, duration_us> Self;

  static const uint16_t totalTicks = (duration_us + timer_period_us / 2) / timer_period_us;

  static Self& getSelf() {
    static Self s = Self();
    return s;
  }

  // True if the pulse is active.
  bool started;
  uint16_t ticks;
  
public:

  static void begin() {
    
    getSelf().started = false;
    
    // We keep the trig pin in Hi-Z state when not active instead of putting them to 0.
    // This way we don't need diodes in case we are charging capacitors with out triggers.
    pin::setInput(false);
  }

  // Called for every tick of the timer.
  static void tick() {
    
    Self& self = getSelf();
    
    if (!self.started)
      return;

    if (self.ticks >= totalTicks) {
      // OK, it's time to finish the pulse, back to Hi-Z state.
      pin::setInput(false);
      self.started = false;
    } else {
      self.ticks++;
    }
  }

  // Begins the pulse.
  static void trig() {
    cli();
    Self& self = getSelf();
    self.ticks = 0;
    self.started = true;
    pin::setHigh();
    pin::setOutput();
    sei();
  }
};

// A basic tick handler for our timer which does nothing.
class NoHandler {
public:
  static void begin() {}
  static void tick() {}
};

// High-res timer allowing to put stuff into the interrupt handler.
template<
  uint16_t period_us,
  typename Handler1, 
  typename Handler2 = NoHandler,
  typename Handler3 = NoHandler,
  typename Handler4 = NoHandler,
  typename Handler5 = NoHandler
>
class Timer {
  
public:

  static void inline handleCOMPA() {    
    Handler1::tick();
    Handler2::tick();
    Handler3::tick();
    Handler4::tick();
    Handler5::tick();
    TIFR |= _BV(OCF0A);
  }

  static void begin() {
    
    TCCR0A = 0;
    TCCR0B = 0;
    
    // Timer mode: Clear Timer on Compare Match (CTC)
    TCCR0A |= (1 << WGM01) | (0 << WGM00);
    TCCR0B |= (0 << WGM02);

    // Enable a compare match A interrupt.
    TIMSK = _BV(OCIE0A);

    // Want an interrupt this many microseconds (assuming single tick of the timer being exactly 1us).
    OCR0A = period_us;

    // Enable the timer with CLK/8 prescaler, so we get 1us per timer tick when running on 8MHz.
    TCCR0B |= (0 << CS02) | (1 << CS01) | (0 << CS00);

    Handler1::begin();
    Handler2::begin();
    Handler3::begin();
    Handler4::begin();
    Handler5::begin();
  }
};

// How often our timer should tick, microseconds. 
// Note that a very small value won't allow the interrupt handler to keep up.
const uint16_t timer_period_us = 50;

typedef PinPulser< FastPin<1>, timer_period_us, 500 > kickPulser;
typedef PinPulser< FastPin<2>, timer_period_us, 500 > snarePulser;
typedef PinPulser< FastPin<3>, timer_period_us, 250 > closedHatPulser;
typedef PinPulser< FastPin<4>, timer_period_us, 8000 > openHatPulser;
typedef PinPulser< FastPin<5>, timer_period_us, 250 > hiTomPulser;

typedef Timer< 
  timer_period_us,
  closedHatPulser,
  openHatPulser,
  snarePulser,
  kickPulser,
  hiTomPulser
> timer;

// Unfortunately an ISR cannot be assigned directly in the template, so have to do it here.
ISR(TIM0_COMPA_vect, ISR_BLOCK) {
  timer::handleCOMPA();
}

// MIDI parser with our event handler triggering notes.
class MIDIMonkey : public MIDIParser<MIDIMonkey> {
  
protected:

  friend MIDIParser<MIDIMonkey>;

  static MIDIMonkey& getSelf() {
    static MIDIMonkey s = MIDIMonkey();
    return s;
  }

  // This is eventually called by the parser, put your handler here.
  static void handleEvent(Event event, uint8_t channel, const uint8_t *args) {
    
    if (channel != 0)
      return;

    if (event == EventNoteOn) {

      uint8_t note = args[0] % 12;

      if (note == NoteF || note == NoteFs) {
        kickPulser::trig();
      } else if (note == NoteG || note == NoteGs) {
        snarePulser::trig();
      } else if (note == NoteA || note == NoteAs) {
        hiTomPulser::trig();
      } else if (note == NoteC || note == NoteCs) {
        closedHatPulser::trig();      
      } else if (note == NoteD || note == NoteDs) {
        openHatPulser::trig();
      }
    }
  }

public:

  static void begin() {
    
    MIDIMonkey& self = getSelf();
      
    MIDIParser<MIDIMonkey>::begin();
  }

  static void check() {

    MIDIMonkey& self = getSelf();
    
    uint8_t b = midi.read(50);
    if (b) {
      MIDIMonkey::handleByte(b);
    }
  }
};

void setup() {
  
  // Digispark's initialization code seems to be messing with the prescaler, let's reset it.
  CLKPR = _BV(CLKPCE);
  CLKPR = 0;
  
  // debugOut.begin();
  // debugOut.println("MIDIMonkey");

  timer::begin();
  MIDIMonkey::begin();
}

void loop() {
  MIDIMonkey::check();
}

