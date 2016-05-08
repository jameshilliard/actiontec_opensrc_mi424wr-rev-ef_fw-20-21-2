/* InputMethodEvent.java -- events from a text input method
   Copyright (C) 1999, 2002 Free Software Foundation, Inc.

This file is part of GNU Classpath.

GNU Classpath is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU Classpath is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Classpath; see the file COPYING.  If not, write to the
Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
02110-1301 USA.

Linking this library statically or dynamically with other modules is
making a combined work based on this library.  Thus, the terms and
conditions of the GNU General Public License cover the whole
combination.

As a special exception, the copyright holders of this library give you
permission to link this library with independent modules to produce an
executable, regardless of the license terms of these independent
modules, and to copy and distribute the resulting executable under
terms of your choice, provided that you also meet, for each linked
independent module, the terms and conditions of the license of that
module.  An independent module is a module which is not derived from
or based on this library.  If you modify this library, you may extend
this exception to your version of the library, but you are not
obligated to do so.  If you do not wish to do so, delete this
exception statement from your version. */


package java.awt.event;

import java.awt.AWTEvent;
import java.awt.Component;
import java.awt.EventQueue;
import java.awt.font.TextHitInfo;
import java.io.IOException;
import java.io.ObjectInputStream;
import java.text.AttributedCharacterIterator;

/**
 * This class is for event generated by change in a text input method.
 *
 * @author Aaron M. Renn <arenn@urbanophile.com>
 * @see InputMethodListener
 * @since 1.2
 * @status updated to 1.4
 */
public class InputMethodEvent extends AWTEvent
{
  /**
   * Compatible with JDK 1.2+.
   */
  private static final long serialVersionUID = 4727190874778922661L;

  /** This is the first id in the range of event ids used by this class. */
  public static final int INPUT_METHOD_FIRST = 1100;

  /** This event id indicates that the text in the input method has changed. */
  public static final int INPUT_METHOD_TEXT_CHANGED = 1100;

  /** This event id indicates that the input method curor point has changed. */
  public static final int CARET_POSITION_CHANGED = 1101;

  /** This is the last id in the range of event ids used by this class. */
  public static final int INPUT_METHOD_LAST = 1101;

  /**
   * The timestamp when this event was created.
   *
   * @serial the timestamp
   * @since 1.4
   */
  private long when;

  /** The input method text. */
  private final transient AttributedCharacterIterator text;

  /** The number of committed characters in the text. */
  private final transient int committedCharacterCount;

  /** The caret. */
  private final transient TextHitInfo caret;

  /** The most important position to be visible. */
  private final transient TextHitInfo visiblePosition;

  /**
   * Initializes a new instance of <code>InputMethodEvent</code> with the
   * specified source, id, timestamp, text, char count, caret, and visible
   * position.
   *
   * @param source the source that generated the event
   * @param id the event id
   * @param when the timestamp of the event
   * @param text the input text
   * @param committedCharacterCount the number of committed characters
   * @param caret the caret position
   * @param visiblePosition the position most important to make visible
   * @throws IllegalArgumentException if source is null, id is invalid, id is
   *         CARET_POSITION_CHANGED and text is non-null, or if
   *         committedCharacterCount is out of range
   * @since 1.4
   */
  public InputMethodEvent(Component source, int id, long when,
                          AttributedCharacterIterator text,
                          int committedCharacterCount, TextHitInfo caret,
                          TextHitInfo visiblePosition)
  {
    super(source, id);
    this.when = when;
    this.text = text;
    this.committedCharacterCount = committedCharacterCount;
    this.caret = caret;
    this.visiblePosition = visiblePosition;
    if (id < INPUT_METHOD_FIRST || id > INPUT_METHOD_LAST
        || (id == CARET_POSITION_CHANGED && text != null)
        || committedCharacterCount < 0
        || (committedCharacterCount
            > (text == null ? 0 : text.getEndIndex() - text.getBeginIndex())))
      throw new IllegalArgumentException();
  }

  /**
   * Initializes a new instance of <code>InputMethodEvent</code> with the
   * specified source, id, text, char count, caret, and visible position.
   *
   * @param source the source that generated the event
   * @param id the event id
   * @param text the input text
   * @param committedCharacterCount the number of committed characters
   * @param caret the caret position
   * @param visiblePosition the position most important to make visible
   * @throws IllegalArgumentException if source is null, id is invalid, id is
   *         CARET_POSITION_CHANGED and text is non-null, or if
   *         committedCharacterCount is out of range
   * @since 1.4
   */
  public InputMethodEvent(Component source, int id,
                          AttributedCharacterIterator text,
                          int committedCharacterCount, TextHitInfo caret,
                          TextHitInfo visiblePosition)
  {
    this(source, id, EventQueue.getMostRecentEventTime(), text,
         committedCharacterCount, caret, visiblePosition);
  }

  /**
   * Initializes a new instance of <code>InputMethodEvent</code> with the
   * specified source, id, caret, and visible position, and with a null
   * text and char count.
   *
   * @param source the source that generated the event
   * @param id the event id
   * @param caret the caret position
   * @param visiblePosition the position most important to make visible
   * @throws IllegalArgumentException if source is null or id is invalid
   * @since 1.4
   */
  public InputMethodEvent(Component source, int id, TextHitInfo caret,
                          TextHitInfo visiblePosition)
  {
    this(source, id, EventQueue.getMostRecentEventTime(), null, 0, caret,
         visiblePosition);
  }

  /**
   * This method returns the input method text. This can be <code>null</code>,
   * and will always be null for <code>CARET_POSITION_CHANGED</code> events.
   * Characters from 0 to <code>getCommittedCharacterCount()-1</code> have
   * been committed, the remaining characters are composed text.
   *
   * @return the input method text, or null
   */
  public AttributedCharacterIterator getText()
  {
    return text;
  }

  /**
   * Returns the number of committed characters in the input method text.
   *
   * @return the number of committed characters in the input method text
   */
  public int getCommittedCharacterCount()
  {
    return committedCharacterCount;
  }

  /**
   * Returns the caret position. The caret offset is relative to the composed
   * text of the most recent <code>INPUT_METHOD_TEXT_CHANGED</code> event.
   *
   * @return the caret position, or null
   */
  public TextHitInfo getCaret()
  {
    return caret;
  }

  /**
   * Returns the position that is most important to be visible, or null if
   * such a hint is not necessary. The caret offset is relative to the composed
   * text of the most recent <code>INPUT_METHOD_TEXT_CHANGED</code> event.
   *
   * @return the position that is most important to be visible
   */
  public TextHitInfo getVisiblePosition()
  {
    return visiblePosition;
  }

  /**
   * This method consumes the event.  A consumed event is not processed
   * in the default manner by the component that generated it.
   */
  public void consume()
  {
    consumed = true;
  }

  /**
   * This method tests whether or not this event has been consumed.
   *
   * @return true if the event has been consumed
   */
  public boolean isConsumed()
  {
    return consumed;
  }

  /**
   * Return the timestamp of this event.
   *
   * @return the timestamp
   * @since 1.4
   */
  public long getWhen()
  {
    return when;
  }

  /**
   * This method returns a string identifying the event. This contains the
   * event ID, the committed and composed characters separated by '+', the
   * number of committed characters, the caret, and the visible position.
   *
   * @return a string identifying the event
   */
  public String paramString()
  {
    StringBuffer s
      = new StringBuffer(80 + (text == null ? 0
                               : text.getEndIndex() - text.getBeginIndex()));
    s.append(id == INPUT_METHOD_TEXT_CHANGED ? "INPUT_METHOD_TEXT_CHANGED, "
             : "CARET_POSITION_CHANGED, ");
    if (text == null)
      s.append("no text, 0 characters committed, caret: ");
    else
      {
        s.append('"');
        int i = text.getBeginIndex();
        int j = committedCharacterCount;
        while (--j >= 0)
          s.append(text.setIndex(i++));
        s.append("\" + \"");
        j = text.getEndIndex() - i;
        while (--j >= 0)
          s.append(text.setIndex(i++));
        s.append("\", ").append(committedCharacterCount)
          .append(" characters committed, caret: ");          
      }
    s.append(caret == null ? (Object) "no caret" : caret).append(", ")
      .append(visiblePosition == null ? (Object) "no visible position"
              : visiblePosition);
    return s.toString();
  }

  /**
   * Reads in the object from a serial stream, updating when to
   * {@link EventQueue#getMostRecentEventTime()} if necessary.
   *
   * @param s the stream to read from
   * @throws IOException if deserialization fails
   * @throws ClassNotFoundException if deserialization fails
   * @serialData default, except for updating when
   */
  private void readObject(ObjectInputStream s)
    throws IOException, ClassNotFoundException
  {
    s.defaultReadObject();
    if (when == 0)
      when = EventQueue.getMostRecentEventTime();
  }
} // class InputMethodEvent
