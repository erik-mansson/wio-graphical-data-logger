/*
Arduino data logger with line chart diagram and file handling in a graphical user interface.

It runs on a Wio Terminal with an analogue Seed Studio Grove sensor
connected in the right socket and optionally a speaker in the left socket.
(Adjusting pin numbers might perhaps make it run on other hardware with a display.)

On the live screen, the up & down arrows and middle button control a threshold value.
The ratio between the measured value and the threshold is shown by a vertical bar on
at the right edge of the screen, and also affects the background colour of the entire screen.
In case sound is enabled, the speaker starts beeping at a rate relating to how
much the threshold is exceeded.

On the live screen, the left & right arrows control the sampling rate, i.e. the number
of samples that are averaged to produce one data point. Actually, a fixed pattern of
several raw analogue readouts are made even per sample, and median-values are used in
order to suppress some of the high-frequency noise that cheap switching power supplies create.

On the Menu screen, the up & down arrows allow selecting a measurement scale,
i.e. how the raw voltage measurements should be transformed for display (and logging).
See the valueFromRaw() function and the variable named sample_transform.
At start-up, the default sample_transform converts readings from the 
Grove GSR (galvanic skin response) sensor 101020052 to conductance (inverse of resistance),
and the Grove moisture sensor 101020008 behaves similarly. For general use, there is
a sample_transform option which simply expresses the analogue input as a voltage.

If an SD-card is available, data can be logged to a text file with a tab-separated values.
Each row contains three columns: a timestamp in seconds, the measured value, and the threshold setting.
The program contains a file browser for navigating and visualizing a logged file,
including the ability to zoom and scroll within the file's diagram. A file can
also be exported to the virtual serial port (a computer connected via USB).
At least some 32 GB cards work although some (old) documentation says max 16 GB.

The files named with automatically incrementing five-digit filenames, organized
in four-letter directories. Rebooting the Wio Terminal means that the next file
will get a new directory, and that the timesetamp clock restarts at 0.

Installation notes:
See https://wiki.seeedstudio.com/Wio-Terminal-Getting-Started/#getting-started for the basics.
For Arduino IDE to open the program, the repository folder needs to be renamed to "wio_graphical_data_logger".
This project depends on the following non-standard-Arduino libraries:
"Seeed Arduino FS" https://wiki.seeedstudio.com/Wio-Terminal-FS-Overview/
"Seeed_Arduino_Linechart" https://wiki.seeedstudio.com/Wio-Terminal-LCD-Linecharts/
The version of the TFT_eSPI library provided out-of-the-box by "Seeduino Wio Terminal"
board in Arduino IDE! Thus you should NOT use the library manager to install TFT_eSPI
(https://wiki.seeedstudio.com/Wio-Terminal-LCD-Graphics/). Otherwise nothing but the screen backlight works!

Created by Erik Maansson, 2024.
https://github.com/erik-mansson/wio-graphical-data-logger
*/
#include <SPI.h> // Defines Serial. Only needed for debugging and exporting file content.
#include <list>
#include <Seeed_FS.h>
#include "SD/Seeed_SD.h"

// Graphics library with some documentation at https://wiki.seeedstudio.com/Wio-Terminal-LCD-Graphics/
// NOTE: Do not install the TFT_eSPI library in Arduino IDE, let it find the version 
// provided by "Seeduino Wio Terminal" board instead! Otherwise nothing but the screen backlight works!
#include <TFT_eSPI.h>
#define SCREEN_WIDTH 320  // (TFT_WIDTH and TFT_HEIGHT are swapped for some reasons)
#define SCREEN_HEIGHT 240  // (TFT_WIDTH and TFT_HEIGHT are swapped for some reasons)
TFT_eSPI tft = TFT_eSPI(SCREEN_HEIGHT, SCREEN_WIDTH);
#define LCD_BACKLIGHT (72Ul) // Control Pin of LCD
#define DIAGRAM_COLOR 0x0017 // Make the diagram line slightly darker than TFT_BLUE.

#include "seeed_line_chart.h" // (Modified to have _format = "%.5G"; to limit y-axis ticks to 5 rather than 6 digits.)

// Menu system and high-level logics:
int menu_level = 1;  // The state of the program, affecting data taking, button functions and what is shown on the screen:
// Generally, WIO_KEY_A (rightmost button above the screen) is a menu/back button, although exactly which mode it goes to varies.
// In modes 0 to 3, measurements keep running regardless of whether a diagram is seen and
// regardless of whether is_recording (to file). The header bar shows at least the current measurement value.
#define MODE_0_DARK 0 // Measure (regardless of whether recording to file). The screen is dark (backlight off) to save power.
#define MODE_1_LIVE 1 // Measure (regardless of whether recording to file). Big live diagram.
#define MODE_2_MENU 2 // Measure (regardless of whether recording to file). Other button functions, settings, can go to browse(B) or help(L/R).
#define MODE_3_HELP 3 // Measure (regardless of whether recording to file). Buttons labels shown, for a mode selectable by L/R.
// The following modes can only be entered if is_recording is false.
#define MODE_4_DIRECTORY_BROWSER 4 // List of folders in the root of the SD-card, buttons to select and enter.
#define MODE_5_FILE_BROWSER 5 // List of files within chosen folder, buttons to select and show or return to directory browser.
#define MODE_6_FILE_DIAGRAM 6 // Big diagram with data loaded from file, buttons to pan, zoom and delete. (TODO: Any way to mark as good/interesting?)
#define MODE_7_FILE_DELETE 7 // Confirm whether to delete the chosen file.
int help_for_mode = MODE_1_LIVE;
bool need_to_update_menu_screen = false;

// For line history chart.
// The example Seeduino Graph_demo_1 doesn't work, most likely
// due to some (version?) incompatibility between Seeduino's customized TFT_eSPI and the normal version of the https://github.com/Bodmer/TFT_eWidget
// Would https://github.com/Seeed-Studio/Seeed_Arduino_Linechart/blob/master/examples/basic/basic.ino work?
#define HISTORY_LENGTH 100  // Number of datapoints shown
doubles data; // Queue with elements of type double to store data points (typedef std::queue<double> doubles; is in "seeed_graphics_define.h").
doubles ref_data;
TFT_eSprite spr = TFT_eSprite(&tft);  // Sprite (apparently needed by libraries/Seeed_Arduino_Linechart/seeed_graphics_base.cpp)
// #define HEADER_HEIGHT 24
#define HEADER_HEIGHT 27
// NOTE: Some width/heigh confusion when using rotation. Apparently the TFT and sprite should be created with transposed coordinates (unrotated).
// #define DIAGRAM_WIDTH 315
#define DIAGRAM_WIDTH 312
// const pix_t DIAGRAM_HEIGHT = SCREEN_HEIGHT - HEADER_HEIGHT;  // Height of the actual line chart
const pix_t DIAGRAM_HEIGHT = SCREEN_HEIGHT - HEADER_HEIGHT + 2;  // Height of the actual line chart (let the unused bottom pixels be outside screen)
const pix_t DIAGRAM_END = DIAGRAM_WIDTH + 3;
#define DIAGRAM_AXIS_WIDTH 290 // TODO: to be tuned (do we have have to guess y-axis tick label length?)

// For this application
uint16_t counter;
const int BUFFER_LENGTH = 42;
char buffer[BUFFER_LENGTH];
uint32_t button_pressed = 0ul; // Datatype of digitalRead(uint32_t pin) according to Seeeduino wiring_digital.h
#define VIRTUAL_BUTTON_PRESS 9999ul
bool show_threshold = false;
bool sound = false;
int playing_tone = 0;
int tone_update_counter = 0;
// For processing sensor data
#define SENSOR_PIN A0  // Whatever Grove sensor is plugged to the rightmost port below the display.
int raw_sample_at_other_time;
#define MAX_AVERAGING 64
int averaging_length = 8;  // the averaging will be done on raw ADC-values before applying any conversion function to physical quantity
long samples_for_averaging[MAX_AVERAGING];  // 32-bit integers are used here for easy summation of more than four 12-bit ADC-values
int sample_index = 0;  // The new value will update samples_for_averaging[sample_index], and increment sample_index modulo MAX_AVERAGING.

char sample_transform = 'c';  // Controls which function is used in valueFromRaw. The default was 'L' when not having a proper GSR sensor module.
#define TRANSFORM_COUNT 8  // The number of characters in TRANSFORMS.
const char TRANSFORMS[] = "LQRVclqr";  // The options for sample_transform, in the order they should be toggled.

double valueFromRaw(double raw) {
  /* Convert from raw ADC-integer to a physically meaningful value for display. */
  double value = raw;
  if (sample_transform >= 'l') {
    // Lowercase letter means reversed scaling
      value = 4095.0 - raw;
  }
  switch (sample_transform) {
    case 'c':
      // Conductance from Grove GSR module, according to https://wiki.seeedstudio.com/Grove-GSR_Sensor/
      // "Use the screw driver to adjust resistor until the serial output is 512 when electrodes are not worn (open circuit).
      //  Resistance = ((1024 + 2Serial_Port_Reading) * 10000) / (512 - Serial_Port_Reading), unit is ohm,
      // Serial_Port_Reading is the value display on Serial Port(between 0~1023)".
      // So when I use 12 bits, it would be:
      // "Use the screw driver to adjust resistor until the serial output is 2048 when electrodes are not worn (open circuit).
      //  Resistance = ((4096 + 2 * raw) * 10000) / (2048 - raw) in [Ohm].
      //  Conductance = (2048 - raw) / ((4096 + 2 * raw) * 10000) in [S]=[1/Ohm] (Siemens)."
      // return (2048.0 - raw) / ((4096.0 + 2.0 * raw) * 1.0E4) * 1E7; // [0.1 uS] = [0.1/MOhm]
      return (2048.0 - raw) * 1000.0 / (4096.0 + 2.0 * raw); // [0.1 uS] = [0.1/MOhm] 
      // The range of values is 500.0 to -165.5 [0.1 uS] and negative shoud be avoided by tuning to 0.0 when open circuit.
    case 'Q':  // Quadratic scaling (squared) from 0 to 1000
    case 'q':  // Reversed version of the above
      // return pow(value / 4095.0, 2.0) * 1000.0;
      return pow(value, 2.0) * 5.96337592674589E-05;
    case 'R':  // Square root scaling from 0 to 1000
    case 'r':  // Reversed version of the above
      // return sqrt(value / 4095.0) * 1000.0;
      return sqrt(value) * 15.626907697949846;
    case 'V':  // Voltage in [mV]:
      return raw * 0.8058608058608059;  // So that max raw 4095 gives a value of 3300 mV.
    // case 'L':  // Linear scaling from 0 to 1000 (for the max 3.3 V input)
    // case 'l':  // Reversed version of the above
    default:
      return value / 4.095;
  }
}

double current_value = 0.0; // (in physical units)
double previous_value = 0.0; // (in physical units)
double threshold_value = valueFromRaw(2047.5); // (in physical units) Default to value for the middle of the scale
double trend_value = 0.0;  // (in physical units) Sum of recent differentials, with exponentially decaying weight
#define TREND_WEIGHT 0.7165313105737893  // exp(-1 / (4 samples lifetime))
// double min_value = valueFromRaw(4095.0);  // For line chart y-range start. TODO: Change to something suitable for phycial quantity
double min_value = 5E3;  // For line chart y-range start. Start with a value larger than normal, so it gets updated by observed minimum.
// #define TREND_THRESHOLD 5.0  // For capacitive moisture sensor
// #define TREND_THRESHOLD 1.0  // The absolute difference (in physical units) within which no trend indicator is shown
// #define TREND_THRESHOLD 0.25  // When scale range is changed to 1000 instead of raw 4096
#define TREND_THRESHOLD 0.5  // But raise the threshold somewhat

// TODO: Tweak or calibrate: The number of sampling iterations per second (even if the sleep is about 0.05 s, some other things take time).
#define EMPIRICAL_ITERATION_TIME 0.058  // [s] (17 Hz) Measured when averaging_length is large (so most samples without diagram update).
#define EMPIRICAL_DIAGRAM_DRAW_TIME 0.045  // [s] Measured slowdown when diagram updated for every sample (averaging_length < 4).


String recording_directory = String(); // Gets assigned the first free 4-letter name if a recording is started, then stays unchanged until the device is rebooted.
String recording_filename = String(); // Within recording_directory, this will be incremented every time a recording is started.
String recording_path = String(); // = recording_directory + recording_filename, this will be incremented every time a recording is started.
bool is_recording = false;
File recording_file; // Within recording_directory, this will be incremented every time a recording is started.
const String ROOT = String("/");
String browser_directory = ROOT; // For file browser GUI. Should NOT include any trailing "/" (but "/" alone for ROOT is OK).

// For file browser GUI, a linked list will have all unique names (subdirectories or files depending on menu_level)
// and a fixed number of entries before and after browser_cursor_name can be shown on the screen.
// How to iterate:
// std::cout << "The list contains:";
// for (std::list<String>::iterator it = browser_file_list.begin(); it != browser_file_list.end(); ++it) { std::cout << ' ' << *it; }
// Alternatively, when no desire to change contents:
// for (std::list<String>::const_iterator it = browser_file_list.cbegin(); it != browser_file_list.cend(); ++it) { std::cout << ' ' << *it; }
std::list<String> browser_file_list = std::list<String>(); 
//int browser_cursor_index = 0;
String browser_cursor_name = String(""); // When empty, create_browser_list() will select the last file (and update browser_cursor_name).
#define LIST_COLUMNS 3
#define LIST_ROWS 8
#define ROW_HEIGHT 24  // Using font 2

// For zooming and scrolling in the diagram of a loaded file. Must be reset to all zeros before opening another file.
long file_point_count = 0;
double file_timestamp_start = 0.0; // [s]
double file_timestamp_end = 0.0; // [s]
double file_zoom_duration = 0.0; // [s] With 0 meaning entire file
double file_zoom_offset = 0.0; // [s] With 0 meaning from start of file



void setup() {
  pinMode(LED_BUILTIN, OUTPUT);  // initialize digital pin LED_BUILTIN as an output.

  analogReadResolution(12);  // The Wio Terminal probably has 12 bit ADC (values from 0 to 4095 instead of 1023), in practice seems more like 11.
  for (int i = 0; i < MAX_AVERAGING; i++) {
    // samples_for_averaging[i] = 0;
    samples_for_averaging[i] = 2047; // middle value
  }
  raw_sample_at_other_time = 2047;

  pinMode(WIO_KEY_A, INPUT_PULLUP);
  pinMode(WIO_KEY_B, INPUT_PULLUP);
  pinMode(WIO_KEY_C, INPUT_PULLUP);
  pinMode(WIO_5S_UP, INPUT_PULLUP);
  pinMode(WIO_5S_DOWN, INPUT_PULLUP);
  pinMode(WIO_5S_LEFT, INPUT_PULLUP);
  pinMode(WIO_5S_RIGHT, INPUT_PULLUP);
  pinMode(WIO_5S_PRESS, INPUT_PULLUP);
  
  pinMode(WIO_BUZZER, OUTPUT);
  pinMode(PIN_WIRE_SCL, OUTPUT);  // The left Grove port (not anlogue) where external speaker should be connected
  
  // Analogue inputs probably range from 0 to 1023 (10 bits)
  // pinMode(WIO_LIGHT, INPUT);
  // pinMode(WIO_MIC, INPUT);

  // Grove sensors. What pin number?
  // The Wio Terminal has an illustration saying PA16 and PA17 for the left Grove port and PB8&PB9 for the right Grove port.
  // Most examples just assume the sensor is on A0 and A0 is mentioned on one of the Grove ports on https://wiki.seeedstudio.com/Wio-Terminal-IO-Overview/ says
  // which also says only one of the two Grove ports on the Wio Terminal is for analogue input, the other is for serial (both can be reconfigured for digital input).
  // It's the right Grove port which has A0 according to "Get Started with Wio Terminal - Seeed Studio Wiki.pdf".
  pinMode(A0, INPUT);

  tft.begin();
  tft.fillScreen(TFT_WHITE);
  // tft.setTextColor(TFT_BLACK, TFT_WHITE); // set background color to automatically first draw a box behind the text (helps clear old text)
  tft.setTextColor(TFT_BLACK); // (but the background drawing is handled explicitly instead now)
  tft.setTextFont(1);
  tft.setTextSize(1);  // Nicer to adjust size by using font 1, 2 or 4 for individual drawString() instad of scaling up by setTextSize().
  tft.setTextDatum(TL_DATUM);  // Place text by (top, left)
  // tft.setTextDatum(TC_DATUM);  // Place text by (top, centre)
  // tft.setTextDatum(MC_DATUM);  // Place text by (top, right)
  // tft.setTextDatum(MC_DATUM);  // Place text by (middle, centre)
  digitalWrite(LCD_BACKLIGHT, HIGH); // Turn on the backlight
  digitalWrite(LED_BUILTIN, LOW); // Turn off the built-in LED
  
  spr.createSprite(SCREEN_WIDTH, DIAGRAM_HEIGHT, 1);
  tft.setRotation(3);
  //spr.setRotation(3);
  
  Serial.begin(115200);
  counter = 0;
  while (!Serial && ++counter < 5) { delay(100); }  // Don't wait forever if no serial connection is available
  // NOTE: SD-card is optional, live mode can run without it
  Serial.print("Initializing SD card... ");
  if (!SD.begin(SDCARD_SS_PIN, SDCARD_SPI)) {
    Serial.println("Failed!");
  } else {
    Serial.println("Successful.");
    // TODO: Could read a configuration file in case SD card is available,
    // to remember sample_transform and threshold_value across restarts.

    if (!SD.exists("/AAAA")) {
      // The file browser expects to have at least one directory. Create the first if it doesn't exist.
      SD.mkdir("AAAA");
      recording_directory = String("/AAAA");  // Special case, to not create AAAB when starting the first recording on first boot
      // NOTE: The plan is that after a new directory will be created for the first recording after every reboot (AAAA, AAAB, AAAC, ... ZZZZ).
      // Then the recordings get numbered filenames within the current directory (0001.tsv, 0002.tsv, ...).
    }
    create_browser_list();
  }
  counter = 0;
}

void loop() {
  /* Top-level program, which runs at every iteration. */
  uint32_t button_previously_pressed = button_pressed;
  if (button_pressed == VIRTUAL_BUTTON_PRESS) {
    // Virtual button press event, to get non-live mode screen shown due to a 
    // button pressed in the PREVIOUS iteration (when changing menu_level within file browser and diagram).
    button_pressed = 0;
  } else {
    checkButtons(); // Updates button_pressed to the key code of ONE button, or 0 if none is currently pressed
    // The event of a new button-press occurs when button_pressed && button_pressed != button_previously_pressed

    if (button_pressed == WIO_KEY_A && button_pressed != button_previously_pressed) {
      // The menu/back-button. It is handled separately, since it does the same thing from many modes.
      if (menu_level == MODE_2_MENU) {
        menu_level = MODE_1_LIVE;
      } else if (menu_level >= MODE_6_FILE_DIAGRAM) {
        menu_level = MODE_5_FILE_BROWSER;
        button_pressed = 0; delay(100); // [ms] To avoid that button handling in showBrowser() causes immediate switch to the parent directory.
        button_previously_pressed = VIRTUAL_BUTTON_PRESS;
      } else { // The menu/back button (A) often leads to MODE_2_MENU
        if (menu_level == MODE_0_DARK || !digitalRead(LCD_BACKLIGHT)) {
          // Turn on the display backlight, coming back from MODE_0_DARK
          digitalWrite(LCD_BACKLIGHT, HIGH);
          // And reset the diagram y-range start (min_value) when turning on.
          min_value = min(current_value, 5E3);  // Start with a value larger than normal, so it gets updated by observed minimum.
          trend_value = 0.0;
        }
        menu_level = MODE_2_MENU;
        need_to_update_menu_screen = true;
      }
    }
  }
  
  // Depending on menu level, run either of the screen-updating subroutines.
  // Each subroutine handles the actions for other buttons than WIO_KEY_A.
  if (menu_level <= MODE_3_HELP) {
    // The live modes where data is being measured and the screen is updated periodically.
    runLiveMode(button_previously_pressed);
    delay(47); // [ms] delay. Rather fast acquisition of raw samples (since median & averaging will normally be applied)
  } else {
    // Non-live modes where the screen only updates in response to button-presses.
    if (button_previously_pressed == VIRTUAL_BUTTON_PRESS || (button_pressed != 0 && button_pressed != button_previously_pressed)) {
      if (menu_level == MODE_6_FILE_DIAGRAM) {
        showFile();
      }
      // NOTE: To further help switching from one mode to another, there is no "else" between these if-statements!
      if (menu_level == MODE_7_FILE_DELETE) {
        confirmDelete();
      }
      if (menu_level == MODE_4_DIRECTORY_BROWSER || menu_level == MODE_5_FILE_BROWSER) {
        showBrowser();
      }
    }
    delay(100); // [ms] delay. Less urgency when just acting on button-presses
  }
}

void checkButtons() {
  if (digitalRead(WIO_KEY_C) == LOW) {
    button_pressed = WIO_KEY_C;
  } else if (digitalRead(WIO_KEY_B) == LOW) {
    button_pressed = WIO_KEY_B;
  } else if (digitalRead(WIO_KEY_A) == LOW) {
    button_pressed = WIO_KEY_A;
  } else if (digitalRead(WIO_5S_UP) == LOW) {
    button_pressed = WIO_5S_UP;
  }  else if (digitalRead(WIO_5S_DOWN) == LOW) {
    button_pressed = WIO_5S_DOWN;
  } else if (digitalRead(WIO_5S_LEFT) == LOW) {
    button_pressed = WIO_5S_LEFT;
  } else if (digitalRead(WIO_5S_RIGHT) == LOW) {
    button_pressed = WIO_5S_RIGHT;
  } else if (digitalRead(WIO_5S_PRESS) == LOW) {
    button_pressed = WIO_5S_PRESS;
  } else {
    button_pressed = 0;
  }
}

void runLiveMode(int button_previously_pressed) {
  /* Acquire and use analogue data. Show either a diagram, menu/settings or help, and handle their button actions. */

  // Handle reading and smoothing of analogue sensor value:
  int raw_sample = analogRead(SENSOR_PIN);
  // Try to reduce noise when non-computer power supply, by always using 
  // median of three near-simultaneous readouts as a single raw sample:
  delayMicroseconds(41); // [us] delay(1); // [ms]
  int raw_sample2 = analogRead(SENSOR_PIN);
  delayMicroseconds(71); // [us]delay(2); // [ms]
  int raw_sample3 = analogRead(SENSOR_PIN);
  raw_sample = median3(raw_sample, raw_sample2, raw_sample3);

  raw_sample = analogRead(SENSOR_PIN);
  raw_sample2 = analogRead(SENSOR_PIN);
  delayMicroseconds(11); // [us] delay(1); // [ms]
  raw_sample3 = analogRead(SENSOR_PIN);
  raw_sample2 = median3(raw_sample, raw_sample2, raw_sample3);

  // Finally use median of the three medians (one median was recorded in the previous iteration, halfway through the diagram update)
  raw_sample = median3(raw_sample, raw_sample2, raw_sample_at_other_time);


  samples_for_averaging[sample_index] = long(raw_sample);
  sample_index = (sample_index + 1) % MAX_AVERAGING;
  counter++;  // unsigned 16-bit integer will wrap around eventually
  if (averaging_length < 4 || (counter % (averaging_length / 2) == 0)) {
    // Update the diagram and display.

    // When averaging, this is not made for every sample (only twice per twice per averaging interval, so 50% overlap of averaged intervals).
    int i_start = (sample_index - averaging_length + MAX_AVERAGING) % MAX_AVERAGING;
    if (counter < averaging_length) {
      i_start = 0; // At start of program there may not be enough history
    }
    int i = i_start;
    int averaged_count = 0;
    long min_selected = -1;  // Allow any unsigned 12-bit value
    long max_selected = 32767;  // Allow any unsigned 12-bit value
    if (averaging_length >= 3) {
      // Find min and max
      min_selected = 2 >> 17;
      max_selected = -1;
      do {
        min_selected = min(min_selected, samples_for_averaging[i]);
        max_selected = max(max_selected, samples_for_averaging[i]);
        i = (i + 1) % MAX_AVERAGING;
      } while (i != sample_index);
      // Exclude min and max (outliers). For averaging_length 3 and 4 this means that the median will be used.
      ++min_selected; // To exclude (one instance of) the found minimum from the averaging-loop.
      --max_selected; // To exclude (one instance of) the found maximum from the averaging-loop.
      if (min_selected > max_selected) {
        // The values were so close that all would be excluded. Then skip the min-constraint.
        min_selected = -1;  // Allow any unsigned 12-bit value
      }

      if (averaging_length >= 16) {
        // Check how many samples are well within the selection (not counting repeated values equal to limit even if they will be included).
        i = i_start;
        // Find min and max within the selection
        int inner_min_selected = max_selected;
        int inner_max_selected = min_selected;
        do {
          if (samples_for_averaging[i] >= min_selected && samples_for_averaging[i] <= max_selected) {
            inner_min_selected = min(inner_min_selected, samples_for_averaging[i]);
            inner_max_selected = max(inner_max_selected, samples_for_averaging[i]);
            averaged_count++;
          }
          i = (i + 1) % MAX_AVERAGING;
        } while (i != sample_index);
        if (averaged_count >= 13) {
          // Proceed to count how many would be selected by the inner selection
          ++inner_min_selected; // To exclude (one instance of) the found minimum from the averaging-loop.
          --inner_max_selected; // To exclude (one instance of) the found maximum from the averaging-loop.
          averaged_count = 0;
          i = i_start;
          do {
            if (samples_for_averaging[i] >= inner_min_selected && samples_for_averaging[i] <= inner_max_selected) {
              averaged_count++;
            }
            i = (i + 1) % MAX_AVERAGING;
          } while (i != sample_index);
          if (averaged_count >= 9 && inner_min_selected < inner_max_selected) {
            // OK to apply the inner selection (in total at least two low and two high samples will now be excluded)
            min_selected = inner_min_selected;
            max_selected = inner_max_selected;
          }
        }
      }

      averaged_count = 0;
      i = i_start;
    }
    long raw_sum = 0.0;
    do {
      if (samples_for_averaging[i] < min_selected) {
        // Exclude this sample. And relax the limit slightly so that in case of multiple equal values only one is excluded.
        --min_selected;
      } else if (samples_for_averaging[i] > max_selected) {
        // Exclude this sample. And relax the limit slightly so that in case of multiple equal values only one is excluded.
        ++max_selected;
      } else {
        // Let this sample contribute to the average
        raw_sum += samples_for_averaging[i];
        averaged_count++;
      }
      i = (i + 1) % MAX_AVERAGING;
    } while (i != sample_index);
    current_value = valueFromRaw(raw_sum / double(averaged_count));

    // The ratio between value and threshold will be used to show background color and optionally play sound.
    double ratio = (current_value + previous_value) / (2.0 * threshold_value);
    ratio = sqrt(max(0.0, ratio)); // Scale as sqrt (or possibly logarithm) to make large ratios perceptually more similar.

    // tft.fillScreen(TFT_WHITE);
    // tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, TFT_WHITE);  // Clear a box for the text
    // Use color based on the ratio of current value and reference value:
    uint16_t color = TFT_WHITE;
    double colormap_fraction = 0.0;
    /* // Colormap from white (ratio<=1.0) via bright yellow to red (ratio>=5.0)
    colormap_fraction = constrain((ratio - 0.75) / 4.25, -1.0, 1.0); */
    // Colormap from white (ratio<=1.0) via bright yellow to red (ratio>=3.0)
    colormap_fraction = constrain((ratio - 0.8660254037844386) / 2.1339745962155614, -0.406, 1.0); // range from -(√0,75)÷(3−√0,75) to +1
    if (ratio >= 1.0) {
      uint8_t green = 216 - uint8_t(colormap_fraction * colormap_fraction * 216.5);
      uint8_t blue = uint8_t(max(0.0, 1.0 - 3.0 * colormap_fraction) * (1.0 - colormap_fraction) * 192.5);
      color = tft.color565(255, green, blue);
    }
    tft.fillRect(0, 0, tft.width(), HEADER_HEIGHT, color);
    
    // Define a trend indicator by classifying (current_value - previous_value) as negative, near zero, positive.
    // Apply exponentially weighted history to smoothen it and let indicator linger a while after sudden change.
    trend_value = (trend_value * TREND_WEIGHT) + (current_value - previous_value);
    tft.drawLine(93, HEADER_HEIGHT / 2, 
                 105, HEADER_HEIGHT / 2 - int(constrain(trend_value / TREND_THRESHOLD, -3, 3)) * 4, TFT_BLUE);
    
    // Show horizontal scale bar for 1 second with the current averaging_length.
    // pix_t w = pix_t(DIAGRAM_AXIS_WIDTH * MAX_SAMPLES_PER_SECOND_MEASURED * 5 / HISTORY_LENGTH);
    // Found better equation to account for the fact that diagram updates are done only every max(1, averaging_length / 2)'th sample.
    double time_per_sample = EMPIRICAL_ITERATION_TIME + EMPIRICAL_DIAGRAM_DRAW_TIME / double(max(1, averaging_length / 2));
    pix_t w = pix_t((DIAGRAM_AXIS_WIDTH * 5) / int(time_per_sample * HISTORY_LENGTH));
    if (averaging_length >= 4) {
      // When averaging at least 4 samples, the diagram is only updated twice per interval
      // Original: w /= (averaging_length / 2);  // To get bar for same time interval
      w /= (averaging_length / 4);  // Double the bar length and show "10 s" instead, to have some precision.
    }
    
    if (menu_level == MODE_1_LIVE) {
      tft.setTextColor(TFT_LIGHTGREY);
      // Show small hints for the A, B and C buttons
      tft.drawString("sound", 3, -1, 1);  // Using font 1
      if (is_recording) {
        tft.drawString("stop", 74, -1, 1);  // Using font 1
      } else {
        tft.drawString("rec", 78, -1, 1);  // Using font 1
      }
      tft.drawString("menu", 160, -1, 1);  // Using font 1
      
      tft.fillRect(DIAGRAM_END - w, HEADER_HEIGHT - 4, w, 3, TFT_LIGHTGREY);
      if (averaging_length >= 4) {
        // When averaging at least 4 samples, the diagram is only updated twice per interval. So the bar is 10 rather than 5 s long.
        tft.drawString("10 s", DIAGRAM_END - 28 - w, HEADER_HEIGHT - 7, 1);  // Using font 1
      } else {
        tft.drawString("5 s", DIAGRAM_END - 20 - w, HEADER_HEIGHT - 7, 1);  // Using font 1
      }
    } else if (menu_level == MODE_2_MENU) {
      // Show small hints for the A, B and C buttons
      tft.setTextColor(TFT_LIGHTGREY);
      tft.drawString("screen", 3, -1, 1);  // Using font 1
      tft.drawString("files", 72, -1, 1);  // Using font 1
      tft.drawString("back", 160, -1, 1);  // Using font 1
    } else { // if (menu_level == MODE_3_HELP)
      tft.setTextColor(TFT_LIGHTGREY);
      tft.drawString("menu", 160, -1, 1);  // Using font 1
    }
    
    // Print some values in the header above the diagram
    tft.setTextColor(TFT_BLACK);
    tft.setTextDatum(TL_DATUM);  // Place text by (top, left)
    // Padding the value with spaces to length 4 (so background color draws over any old value).
    // snprintf(buffer, BUFFER_LENGTH, "%4.1f  %c", current_value, trend);
    snprintf(buffer, BUFFER_LENGTH, "%4.1f", current_value);
    tft.drawString(buffer, 3, 5, 4);  // Using font 4 (large)
    snprintf(buffer, BUFFER_LENGTH, "%+4.1f thr%4.1f Avg.%i %c %s", trend_value, threshold_value,
             averaging_length, sample_transform, sound ? "sound" : "    ");
    tft.drawString(buffer, 112, 4, 2);  // Using font 2
    
    if (is_recording) {  // Show name of file we're recording to
      tft.drawString(recording_path, 112, HEADER_HEIGHT - 7, 1);  // Using font 1
    }
    
    if (menu_level < MODE_2_MENU) {
      // When not showing menu or help, set ratio-based color as background for the diagram
      spr.fillSprite(color);
    } else {
      if (need_to_update_menu_screen || button_pressed || button_previously_pressed) {
        // Spend time drawing the update only after changes, not on every update (since no diagram is shown here).
        // tft.fillRect(0, HEADER_HEIGHT, SCREEN_WIDTH, SCREEN_HEIGHT - HEADER_HEIGHT, TFT_WHITE);
        spr.fillSprite(TFT_WHITE);
        spr.pushSprite(0, HEADER_HEIGHT);  // Request the (now just white) diagram container to be shown, at (left, top).
        need_to_update_menu_screen = false;
      }
      if (menu_level == MODE_3_HELP) {
        showHelp();
      } else {
        showMenu();
      }
      // Stop sounds while in the menu (since changing scale is likely to get value higher than threshold)
      // NOTE: The program seems to freze in case noTone() is called before a tone() has been played.
      if (playing_tone != 0) {
        noTone(PIN_WIRE_SCL);
        playing_tone = 0;
      }
    }

    // Append the data point (this can take some time)
    if (data.size() >= HISTORY_LENGTH) {
      data.pop(); // Remove the oldest point before appending a new
      ref_data.pop();
    }
    if (current_value < min_value) {
      min_value = current_value; // To not necessarily have 0.0 as bottom limit of the diagram y-axis (the top is autoscaled anyway)
    }
    data.push(current_value);
    ref_data.push(threshold_value);

    // Do another raw readout here, with some time offset from the main one
    raw_sample = analogRead(SENSOR_PIN);
    // Try to reduce noise when non-computer power supply, by always using 
    // median of three near-simultaneous readouts as a single raw sample:
    delayMicroseconds(41); // [us] delay(1); // [ms]
    raw_sample2 = analogRead(SENSOR_PIN);
    raw_sample3 = analogRead(SENSOR_PIN);
    raw_sample_at_other_time = median3(raw_sample, raw_sample2, raw_sample3);

    if (menu_level < MODE_2_MENU) {
      // Show the live diagram  (the menu is shown higher up, to avoid blinking).
      // Settings for the diagram (see https://wiki.seeedstudio.com/Wio-Terminal-LCD-Linecharts/ )
      line_chart content = line_chart(2, 0); // (left, top) where the line graph begins (y-axis will use some space internally)
      content // The following methods return the object again so they can be chained like .a().b().c()...:
              .tick(2) // This affects t-tick length and x-axis size on display
              .x_skip_tick(0.0f) // DEBUG what does this do? Affects x step size and thus diagram range
              .width(DIAGRAM_WIDTH) // TODO DEBUG: When DIAGRAM_WIDTH is greater than 295 px the latest points are not shown! (As if hidden by white.)
              .height(DIAGRAM_HEIGHT) // Height of the actual line chart
              .based_on(min_value - 0.01) // Low starting point of y-axis. Subtract something to handle bug of exact min being behind x-axis.
              .show_circle(false); // Disable drawing of points (draw just line)
      if (show_threshold) {
        content.value({ref_data, data}); // Show also the reference value
        content.color(TFT_LIGHTGREY, DIAGRAM_COLOR); // Choose line colors
      } else {
        content.value(data); // The data (queue) to show (gets copied to an array by this call, in content._value which is private but accesible )
        content.color(DIAGRAM_COLOR); // Choose line color
      }
      content // The following methods return the object again so they can be chained like .a().b().c()...:
              .y_role_thickness(2)
              .x_role_color(TFT_LIGHTGREY)  // Color for y-axis
              .x_tick_color(TFT_LIGHTGREY)  // Color for y-axis tick labels
              .y_role_color(TFT_LIGHTGREY)  // Color for y-axis
              .y_tick_color(TFT_LIGHTGREY)  // Color for y-axis tick labels
              // Note: to change the pan_color that seems to control the dotted lines for ticks, seeeded_graphics_define.h would need to be modified.
              .draw();
      spr.pushSprite(0, HEADER_HEIGHT);  // Request the diagram to be shown, at (left, top).
      
      // Draw the ratio also as a vertical progressbar
      // colormap_fraction ranges from -0.406 to +1.0 ==> 1 - colormap_fraction ranges from 0 to 1.406
      w = int(max(0.0, (1.0 - colormap_fraction) / 1.4058274195579777 * SCREEN_HEIGHT));  // 1 + (√0,75)÷(3−√0,75)
      tft.fillRect(DIAGRAM_END + 2, w, 3, SCREEN_HEIGHT - w, DIAGRAM_COLOR);
      
      if (sound) {
        // Beep when the value exceeds the threshold, pulsing more often when the ratio becomes high.
        // Frequency-controlled tone on external speaker connected to the left Grove port.
        int new_tone = int(constrain(192.0 + 72.0 * ratio, 220.0, 990.0));
        if (ratio >= 1.0) { // Should play something
          // tone(PIN_WIRE_SCL, frequency, 100);  // play non-blocking for 100 ms
          if (ratio <= 4.0) {
            if (playing_tone != 0) {
              noTone(PIN_WIRE_SCL); // Stop a previous continuous tone
              playing_tone = 0;
            }
            // if ((tone_update_counter++) % int(8.01 / ratio) == 0) {
            if ((tone_update_counter++) % int(7.01 / ratio) == 0) {
              // tone(PIN_WIRE_SCL, new_tone, 80);  // play non-blocking, only for 80 ms per diagram update
              tone(PIN_WIRE_SCL, new_tone, 64);  // play non-blocking, only for 64 ms per diagram update
            }
          } else if (playing_tone != 0 && (playing_tone >= new_tone - 40) && (playing_tone <= new_tone + 40)) {
            // Keep playing the similar previous tone (to avoid many minor interruptions)
            // tone(PIN_WIRE_SCL, playing_tone);  // play non-blocking, continuously
            ;
          } else {
            tone(PIN_WIRE_SCL, new_tone);  // play non-blocking, continuously
            playing_tone = new_tone;
          }
        } else {
          // NOTE: The program seems to freze in case noTone() is called before a tone() has been played.
          if (playing_tone != 0) {
            noTone(PIN_WIRE_SCL);
            playing_tone = 0;
          }
        }
      }
    }

    if (is_recording) {
      // Write a data point to the file (timestamp, value and threshold formatted as tab-separated text).
      snprintf(buffer, BUFFER_LENGTH, "%.2f\t%.2f\t%.1f\n", double(millis()) * 0.001, current_value, threshold_value);
      recording_file.print(buffer);
    }
    
    previous_value = current_value; // (in physical units)
    
  }  // End of diagram & display update

  if (!button_pressed) {
    ; // No button pressed
  } else if (button_pressed != button_previously_pressed) {
    // A newly pressed button
    
    if (menu_level == MODE_3_HELP) {
      if (button_pressed == WIO_KEY_B || button_pressed == WIO_KEY_C) {
        // Let the B and C buttons (which have no other function here) leave the help and return to main menu.
        menu_level = MODE_2_MENU;
        need_to_update_menu_screen = true;
      } else if (button_pressed == WIO_5S_LEFT) {
        // The left/right buttons step through multiple help screens.
        help_for_mode = max(MODE_1_LIVE, help_for_mode - 2); // Only modes 1, 2, 4 and 6 have help.
        need_to_update_menu_screen = true;
      } else if (button_pressed == WIO_5S_RIGHT) {
        help_for_mode = help_for_mode - (help_for_mode % 2) + 2; // Only modes 1, 2, 4 and 6 have help.
        if (help_for_mode > MODE_6_FILE_DIAGRAM) { // Wrap around
          help_for_mode = MODE_1_LIVE;
        }
        need_to_update_menu_screen = true;
      }
    } ///////////////////////////////////
    else if (menu_level == MODE_2_MENU) {
      
      if (button_pressed == WIO_KEY_B) {
        if (is_recording) {
          // Not allow to start listing directories & files while recording (believed that the Arduino can't have two files open at the same time).
          tft.fillScreen(TFT_YELLOW);
          tft.setTextColor(TFT_RED);
          tft.drawString("Can not browse files", 20, 80, 4);  // Using font 4
          tft.drawString("while recording to a file.", 20, 120, 4);  // Using font 4
          need_to_update_menu_screen = true;
          // Play an error sound
          tone(PIN_WIRE_SCL, 280, 70);  // play 280 Hz non-blocking for 70 ms
          delay(70); // wait 70 ms
          tone(PIN_WIRE_SCL, 260, 100);  // play 260 Hz non-blocking for 100 ms
        } else {
          menu_level = MODE_4_DIRECTORY_BROWSER;
          browser_directory = ROOT;
          browser_cursor_name = String(""); // Will cause create_browser_list() to select the last file.
          create_browser_list();
          button_pressed = VIRTUAL_BUTTON_PRESS; // Acts as a virtual button press event in the next loop(), to get the browser shown then.
        }
      } else if (button_pressed == WIO_KEY_C) {
        // Turn off the display backlight
        digitalWrite(LCD_BACKLIGHT, LOW);
        menu_level = MODE_0_DARK;
      } else if (button_pressed == WIO_5S_UP || button_pressed == WIO_5S_DOWN) {
        // Toggle between transform functions to map raw samples to shown quantity.
        int p = 0;
        // Find the current transform's index in TRANSFORMS.
        for (; p < TRANSFORM_COUNT && TRANSFORMS[p] != sample_transform; p++) {; }
        // If not found, p will equal the TRANSFORM_COUNT which is congruent with 0 modulo TRANSFORM_COUNT.
        if (button_pressed == WIO_5S_DOWN) {
          sample_transform = TRANSFORMS[(p + 1) % TRANSFORM_COUNT];  // Pick the next transform
        } else {
          sample_transform = TRANSFORMS[(p + TRANSFORM_COUNT - 1) % TRANSFORM_COUNT];  // Pick the preceeding transform
        }
        need_to_update_menu_screen = true;
        // Clear the diagram history
        while (!data.empty()) { data.pop(); }
        while (!ref_data.empty()) { ref_data.pop(); }
        min_value = 5E3;  // Start with a value larger than normal, so it gets updated by observed minimum.
        trend_value = 0.0;

      } else if (button_pressed == WIO_5S_PRESS) {
        // Toggle whether the threshold is shown as a curve in the diagram.
        show_threshold = !show_threshold;
        tft.fillScreen(TFT_WHITE);
        tft.setTextColor(TFT_BLACK);
        if (show_threshold) {
          tft.drawString("Showing threshold curve", 10, 150, 4);  // Using font 4
        } else {
          tft.drawString("Hiding threshold curve", 10, 150, 4);  // Using font 4
        }
        need_to_update_menu_screen = true;
      } else if (button_pressed == WIO_5S_LEFT || button_pressed == WIO_5S_RIGHT) {
        menu_level = MODE_3_HELP;
        need_to_update_menu_screen = true;
        help_for_mode = MODE_1_LIVE; // Start with the default help page regardless of whether left or right was pressed
      }

    } ///////////////////////////////////
    else { // MODE_0_DARK or MODE_1_LIVE

      if (button_pressed == WIO_KEY_B) {
        toggleRecording();
      } else if (button_pressed == WIO_KEY_C) {
        // Toggle sound on or off (or possibly between sound modes).
        if (sound && playing_tone != 0) {
          noTone(PIN_WIRE_SCL);
          playing_tone = 0;
        }
        sound = !sound;
        // TODO: Consider changing the function so that from MODE_0_DARK this turns on the screen instead of toggling sound.
      } else if (button_pressed == WIO_5S_RIGHT) {
        // Fewer averaged samples per point, faster diagram.
        averaging_length = constrain(averaging_length / 2, 1, MAX_AVERAGING);
      } else if (button_pressed == WIO_5S_LEFT) {
        // More averaged samples per point, slower diagram.
        if (averaging_length < 4) {
          averaging_length = constrain(averaging_length + 1, 1, MAX_AVERAGING);
        } else {
          averaging_length = constrain(averaging_length * 2, 1, MAX_AVERAGING);
        }
      } else if (button_pressed == WIO_5S_PRESS) {
        // Click joystick: Set the reference value to the current value.
        threshold_value = (previous_value + current_value) / 2.0;
      } else if (button_pressed == WIO_5S_UP) {
        // Up & Down joystick: change reference level for what's high or low conductance, affecting shown bar and how the buzzer sounds (if enabled).
        // Since the loop runs rather quickly now, it's difficult to do a "single" click and smaller changer per iteration is suitable.
        threshold_value += 0.01 * current_value + 0.125 * TREND_THRESHOLD;
      } else if (button_pressed == WIO_5S_DOWN) {
        threshold_value -= 0.01 * current_value + 0.125 * TREND_THRESHOLD;
        if (threshold_value < 0.01) {
          threshold_value = 0.01;
        }
      }
    }

  } else {
    // A button is being held pressed for a longer time (button_pressed == button_previously_pressed)
    if (menu_level <= MODE_1_LIVE) {
      if (button_pressed == WIO_5S_UP) {
        // Up & Down joystick: change reference level for what's high or low conductance, affecting shown bar and how the buzzer sounds (if enabled).
        threshold_value += 0.01 * current_value + TREND_THRESHOLD + 0.02 * threshold_value;
      } else if (button_pressed == WIO_5S_DOWN) {
        threshold_value -= 0.01 * current_value + TREND_THRESHOLD + 0.015 * threshold_value;
        if (threshold_value < 0.01) {
          threshold_value = 0.01;
        }
      }
    }
  }
}

int median3(int a, int b, int c) {
  /* Return the median of three integers. */
  int tmp = min(b, c);
  if (a < tmp) {  // a is smallest, tmp is the median
    return tmp;
  } else {
    tmp = max(b, c);
    if (a > tmp) {  // a is largest, tmp is the median
      return tmp;
    }
  } // otherwise: a is the median
  return a;
}

void showMenu() {
  /* Show current transform scale name, and some basic button labels to show user how to reach other modes. */
  // Label the most important buttons
  tft.setTextColor(TFT_BLUE);
  tft.drawString("|_Sound__|_Recording___|_Menu_|", 4, 25, 2);
  tft.drawString("|_Screen_|_File_browser_|_Back_|", 4, 41, 2);
  if (show_threshold) {
    tft.drawString("(Centre) to hide threshold curve", 142, 204, 2);
  } else {
    tft.drawString("(Centre) to show threshold curve", 142, 204, 2);
  }
  tft.drawString("> Right > for help screens", 142, 220, 2);

  tft.setTextColor(TFT_BLACK);
  tft.drawString("Measurement scale =", 16, 80, 2);
  if (sample_transform == 'c') {
    tft.drawString("Grove GSR conductance in [0.1 uS]", 24, 100, 2);
  } else if (sample_transform == 'V') {
    tft.drawString("linear voltage, 0 to 3300 [mV]", 24, 100, 2);
  } else {
    char transform_descripion[28];
    switch (sample_transform) {
      case 'Q':
      case 'q':  // Reversed version of the above
        strcpy(transform_descripion, "quadratic");
        break;
      case 'R':  // Square root scaling from 0 to 1000
      case 'r':  // Reversed version of the above
        strcpy(transform_descripion, "square root");
        break;
      // case 'L':  // Linear scaling from 0 to 1000
      // case 'l':  // Reversed version of the above
      default:
        strcpy(transform_descripion, "linear");
        break;
    }
    if (sample_transform >= 'l') {  // Lowercase letter means reversed scaling
      snprintf(buffer, BUFFER_LENGTH, "reversed %s, 1000 to 0(3.3 V)", transform_descripion);
    } else {
      snprintf(buffer, BUFFER_LENGTH, "%s, 0 to 1000(when 3.3 V)", transform_descripion);
    }
    tft.drawString(buffer, 24, 100, 2);
  }
  tft.drawString("Can be changed by up & down buttons.", 16, 120, 2);

  delay(16); // Try to avoid some flickering that does not happen in MODE_1_LIVE
}

void showHelp() {
  /* Draw button labels and other help instead of the live diagram. */
  tft.setTextColor(TFT_LIGHTGREY);
  tft.drawString(">Next> ", 284, HEADER_HEIGHT, 1);

  // Illustrate the buttons by boxes and an ellipse
  tft.fillRect(4, 46, 64, 32, TFT_BLUE);
  tft.fillRect(70, 46, 72, 32, TFT_BLUE);
  tft.fillRect(144, 46, 64, 32, TFT_BLUE);
  tft.fillEllipse(208, 208, 115, 35, TFT_BLUE);
  switch (help_for_mode) {
    case MODE_1_LIVE:
      tft.setTextColor(TFT_WHITE);
      tft.drawString("Toggle   Start/stop   Menu", 16, 46, 2);
      tft.drawString("sound    recording", 16, 60, 2);
      tft.drawString("Raise threshold", 163, 181, 2);
      tft.drawString("Slower    Set threshold    Faster", 104, 199, 2);
      tft.drawString("Lower threshold", 163, 217, 2);
      tft.setTextColor(TFT_BLACK);
      tft.drawString("Help for: Live diagram", 8, 28, 2);
      tft.drawString("Made by Erik Maansson 2024", 34, 96, 2);
      tft.drawString("For Wio Terminal with Grove sensor in", 34, 112, 2);
      tft.drawString("the right socket and speaker in the left.", 34, 128, 2);
      break;
    case MODE_2_MENU:
      tft.setTextColor(TFT_WHITE);
      tft.drawString("Toggle    File        Back", 14, 46, 2);
      tft.drawString("screen    browser    to live", 14, 60, 2);
      tft.drawString("Previous scale", 163, 181, 2);
      tft.drawString("Help    Show threshold?    Help", 111, 199, 2);
      tft.drawString("Next scale", 173, 217, 2);
      tft.setTextColor(TFT_BLACK);
      tft.drawString("Help for: Main menu", 8, 28, 2);
      break;
    case MODE_4_DIRECTORY_BROWSER:
      tft.setTextColor(TFT_WHITE);
      tft.drawString("Back      Back        Menu", 14, 46, 2);
      tft.drawString("or menu   or menu", 14, 60, 2);
      tft.drawString("Select previous", 160, 181, 2);
      tft.drawString("Select left    Open    Select right", 102, 199, 2);
      tft.drawString("Select next", 175, 217, 2);
      tft.setTextColor(TFT_BLACK);
      tft.drawString("Help for: File browser", 8, 28, 2);
      break;
    case MODE_6_FILE_DIAGRAM:
      tft.setTextColor(TFT_WHITE);
      tft.drawString("Delete     Export     Back", 14, 46, 2);
      tft.drawString("file        to serial   to list", 14, 60, 2);
      tft.drawString("Zoom out", 181, 181, 2);
      tft.drawString("<Scroll  Show threshold?  Scroll>", 103, 199, 2);
      tft.drawString("Zoom in", 184, 217, 2);
      tft.setTextColor(TFT_BLACK);
      tft.drawString("Help for: File diagram", 8, 28, 2);
      break;
  }
  delay(40); // Try to avoid some flickering that does not happen in MODE_1_LIVE
}

void showBrowser() {
  /* Show directory or file browser, and handle its button actions. 

  NOTE: This relies on create_browser_list() having been called when
  the menu_level was changed. The existing list is simply shown here.
  */
  // Handle buttons (before updating screen)
  bool has_multiple_columns = browser_file_list.size() > LIST_ROWS;
  int steps = 1;
  if (button_pressed == WIO_5S_DOWN || (has_multiple_columns && button_pressed == WIO_5S_RIGHT)) {
    // Move list cursor to the next name (or next column's name) at the same level
    if (browser_file_list.size() == 0) {
      browser_cursor_name = String();
    } else {
      String old_cursor_name = browser_cursor_name;
      browser_cursor_name = browser_file_list.front(); // Fallback in case old_cursor_name is not found or if it is the last entry of the list
      for (std::list<String>::const_iterator it = browser_file_list.cbegin(); it != browser_file_list.cend(); ++it) {
        if ((*it).equals(old_cursor_name)) { // Found the entry that the cursor points to.
          if (button_pressed == WIO_5S_RIGHT) {
            steps = LIST_ROWS;  // Move selection one column to the right
          }
          while (--steps >= 0) {
            ++it; // Move cursor to the next
            if (it != browser_file_list.cend()) {
              browser_cursor_name = *it;
            } else {
              break;
              // if the old cursor was found at the end, the new cursor will be browser_file_list.front()
            }
          }
          break;
        }
      }
    }
  } else if (button_pressed == WIO_5S_UP || (has_multiple_columns && button_pressed == WIO_5S_LEFT)) {
    // Move list cursor to the preceeding name (or the previous column's name) at the same level
    if (browser_file_list.size() == 0) {
      browser_cursor_name = String();
    } else {
      String old_cursor_name = browser_cursor_name;
      browser_cursor_name = browser_file_list.back(); // Fallback in case old_cursor_name is not found or if it is the first entry of the list
      for (std::list<String>::const_reverse_iterator it = browser_file_list.crbegin(); it != browser_file_list.crend(); ++it) {
        if ((*it).equals(old_cursor_name)) { // Found the entry that the cursor points to.
          if (button_pressed == WIO_5S_LEFT) {
            steps = LIST_ROWS;  // Move selection one column to the left
          }
          while (--steps >= 0) {
            ++it; // Move cursor to the preceeding (because this is a backwards iterator)
            if (it != browser_file_list.crend()) {
              browser_cursor_name = *it;
            } else {
              break;
              // if the old cursor was found at the start, the new cursor will be browser_file_list.back()
            }
          }
          break;
        }
      }
    }
  } else if (button_pressed == WIO_5S_PRESS || button_pressed == WIO_5S_RIGHT) {
    // (WIO_5S_RIGHT does not have this effect when there are multiple columns.)
    // TODO: Nested directories can be allowed by basing decision on File entry.isDirectory(), instead of menu_level
    if (menu_level == MODE_5_FILE_BROWSER) {
      // Clicked to open a file, from list of files in a directory.
      menu_level = MODE_6_FILE_DIAGRAM;
      button_pressed = VIRTUAL_BUTTON_PRESS; // Acts as a virtual button press event in the next loop(), to get the browser shown then.
      resetFileZoom();
      return;
    } else { // assume MODE_4_DIRECTORY_BROWSER
      // Clicked to open a directory, from list of (top level) directories.
      browser_directory = String("/") + browser_cursor_name; // NOTE: Must not have traling "/";
      menu_level = MODE_5_FILE_BROWSER;
      browser_cursor_name = String(""); // Will cause create_browser_list() to select the last file.
      create_browser_list();
      
      // TODO: What to do here to get the disply code to run from loop() even if then not new button press? Call the function directly here?
      button_pressed = VIRTUAL_BUTTON_PRESS; // Acts as a virtual button press event in the next loop(), to get the browser shown then.
    }
  } else if (button_pressed == WIO_5S_LEFT || button_pressed == WIO_KEY_B || button_pressed == WIO_KEY_C) {
    // (WIO_5S_LEFT does not have this effect when there are multiple columns.)
    if (menu_level == MODE_5_FILE_BROWSER) {
      // Go to parent directory
      menu_level = MODE_4_DIRECTORY_BROWSER;
      String name_to_select = browser_directory.substring(1);  // The directory we came from, without leading (and trailing) "/" characters
      browser_directory = ROOT;  // Since only one level of directories is supported, the parent is simply ROOT.
      browser_cursor_name = String(""); // Will cause create_browser_list() to select the last file, as fallback.
      create_browser_list();
      // Try to set cursor to the child directory we came from
      for (std::list<String>::const_iterator it = browser_file_list.cbegin(); it != browser_file_list.cend(); ++it) {
        if ((*it).equals(name_to_select)) { // Confirms that name_to_select exists
          browser_cursor_name = name_to_select;
          break;
        }
      }
    } else if (menu_level == MODE_4_DIRECTORY_BROWSER) {
      // Return to menu if trying to go to parent of ROOT (redundant, as WIO_KEY_A would do so too)
      menu_level = MODE_2_MENU;
      need_to_update_menu_screen = true;
      return;
    }
  }
  
  int i = 0;
  for (std::list<String>::const_iterator it = browser_file_list.cbegin(); it != browser_file_list.cend(); ++it) {
    if ((*it).equals(browser_cursor_name)) { // Found the selected name
      break;
    } else {
      ++i;
    }
  }
  int first_to_show = max(int(0), i - int(LIST_ROWS) / 2 - int(LIST_COLUMNS >= 3 ? LIST_ROWS : 0));
  if (browser_file_list.size() < first_to_show + LIST_ROWS * LIST_COLUMNS) {
    // The list ends before the avaiable space, so try starting earlier in the list.
    first_to_show = max(int(0), int(browser_file_list.size()) - LIST_ROWS * LIST_COLUMNS);
  }
  
  tft.fillScreen(TFT_WHITE);
  // Show small hints for the (A,) B and C buttons
  tft.setTextColor(TFT_LIGHTGREY);
  if (menu_level == MODE_5_FILE_BROWSER) {
    tft.drawString("directory", 70, -1, 1);  // Using font 1
  }
  tft.drawString("menu", 162, -1, 1);  // Using font 1
  tft.setTextColor(TFT_BLACK);
  tft.drawString(menu_level == MODE_4_DIRECTORY_BROWSER ? "Directory browser" : "File browser in", 2, 7, 2);  // Using font 2
  tft.drawString(browser_directory, 118, 7, 2);  // Using font 2

  if (browser_file_list.size() == 0) {
    tft.drawString("(empty directory)", 110, 24 - 16 + ROW_HEIGHT * (LIST_ROWS / 2), 2);  // Using font 2
  }
  
  i = 0;
  for (std::list<String>::const_iterator it = browser_file_list.cbegin(); it != browser_file_list.cend(); ++it) {
    if (i >= first_to_show) { 
      int column = (i - first_to_show) / LIST_ROWS;
      if (column >= LIST_COLUMNS) { break; }
      int row = (i - first_to_show) % LIST_ROWS;
      if ((*it).equals(browser_cursor_name)) {
        // Draw a box behind the selected name
        tft.fillRect(100 * column, 24 + ROW_HEIGHT * row, 100, ROW_HEIGHT - 1, 0xC61F);  // light blue (old: TFT_YELLOW)
      }
      tft.drawString(*it, 5 + 100 * column, 26 + ROW_HEIGHT * row, 2);  // Using font 2
    }
    ++i;
  }
}

void create_browser_list() {
  /* Build a list of directories or files from the SD card, within the browser_directory. */
  // NOTE: browser_directory must not have trailing "/" (except when it is ROOT), otherwise the list turns out empty!
  bool show_directories = menu_level < MODE_5_FILE_BROWSER;
  browser_file_list.clear();
  File directory = SD.open(browser_directory);
  File entry;
  while (true) {
    entry = directory.openNextFile();
    if (!entry) {  // End of file list
      break;
    }
    if (entry.isDirectory() == show_directories) { // The entry is of the correct type
      String name = String(entry.name()); // NOTE: This is the full path, including parent_directory!
      if (name.length() > browser_directory.length() + 1) {
        if (browser_directory.endsWith("/")) { // This case is only useful for ROOT
          name = name.substring(browser_directory.length());
        } else {
          name = name.substring(browser_directory.length() + 1);
        }
      }
      if (name.length() >= 2 && name.charAt(0) != '.') {
        // browser_file_list.push_back(name); // appends given object (might require that the .name() is a String rather than a char[])
        browser_file_list.emplace_back(name);  // appends String object constructed from given argument
      } /* else {
        // Consider directory or file with name starting with '.' to be hidden.
      } */
    }
  }
  directory.close();
  browser_file_list.sort(); // Necessary, since the file-system order is not always alphanumeric
  if (browser_cursor_name.length() == 0 && browser_file_list.size() > 0) {
    // If nothing else is selected, select the last name
    browser_cursor_name = browser_file_list.back();
  }
}

String find_last_recording_name(String parent_directory, bool show_directories) {
  /* Find last directory name or file name, in the series used for recording file names. */
  // NOTE: parent_directory must not have trailing "/" (except when it is ROOT), otherwise the list turns out empty!
  
  // TODO: This assumes that the names are returned in string-sorted order, which is not true! 
  // Can it be solved by keeping max(last_name, entry.name())?

  File directory = SD.open(parent_directory);
  String last_name = String("");
  while (File entry = directory.openNextFile()) {
    String name = entry.name(); // NOTE: This is the full path, including parent_directory!
    if (parent_directory.endsWith("/")) {
      name = name.substring(parent_directory.length());
    } else {
      name = name.substring(parent_directory.length() + 1);
    }
    if (show_directories) {
      // The length is 5 when including the leading '/' character in "/AAAA"
      if (entry.isDirectory() && name.length() == 4 && isUpperCase(name.charAt(0))) {
        last_name = name; // A valid directory name
      }
    } else {
      if (!entry.isDirectory() && (name.endsWith(".tsv") || name.endsWith(".TSV"))) {
        last_name = name; // A valid filename
      }
    }
  }
  directory.close();
  return last_name;
}

void toggleRecording() {
  /* Start or stop recording of data to a file, and play tones to indicate what was done.
  
  NOTE: The plan is that after a new directory will be created for the first recording
  after every reboot (AAAA, AAAB, AAAC, ... ZZZZ).
  Then the recordings get numbered filenames within the current directory (0001.tsv, 0002.tsv, ...).
  */
  tft.fillScreen(TFT_WHITE);
  tft.setTextColor(TFT_BLACK);
  if (is_recording) {
    // Stop recording
    Serial.println("Stopping recording"); // DEBUG
    tft.drawString("Recording stopped", 20, 100, 4);  // Using font 4
    is_recording = false;
    recording_file.flush();
    delay(25);
    recording_file.close();
    digitalWrite(LED_BUILTIN, LOW); // Turn off the built-in LED
    Serial.println("Recording stopped.");
    // Frequency-controlled tone on external speaker connected to the left Grove port
    tone(PIN_WIRE_SCL, 440, 140); delay(140);  // play 440 Hz non-blocking but also wait for it during 140 ms
    tone(PIN_WIRE_SCL, 330, 140);  // play 330 Hz non-blocking for 140 ms
    return;
  }
  
  // Prepare to create new file for recording
  
  if (recording_directory.length() == 0) {
    // This is the first recording since the device was rebooted. Need to create a directory too.
    // It will be assigned the first free 4-letter name in ROOT, then stays in use until the device is rebooted.
    String last_directory = ROOT + find_last_recording_name(ROOT, true);
    if (last_directory.length() == 0) {
      recording_directory = String("/AAAA"); // (The leading slash is not needed by Arduino SD library, but used here for consistency)
    } else {
      recording_directory = last_directory;
      // Increment directory name beyond the last existing, in order like /AAAA, /AAAB ...
      for (int p = 4; p >= 1; --p) {
        char c = recording_directory.charAt(p);
        if (c == 'Z') {
          recording_directory.setCharAt(p, 'A');  // Roll over from 'Z' to 'A', loop will continue with preceeding character
        } else if (c < 'A') {
          Serial.print("Invalid directory name: "); Serial.println(recording_directory);
          tft.fillScreen(TFT_YELLOW);
          tft.setTextColor(TFT_RED);
          tft.drawString("Invalid directory name: ", 20, 80, 4);  // Using font 4
          tft.drawString(recording_directory, 20, 120, 4);  // Using font 4  
          return;
        } else {
          recording_directory.setCharAt(p, c + 1);  // Increment and exit the loop
          break;
        }
      }
    }
  }

  if (!SD.exists(recording_directory)) {
    Serial.print("mkdir "); Serial.print(recording_directory); delay(50); // DEBUG
    recording_filename = String("0000.tsv");
    SD.mkdir(recording_directory);
    Serial.println("; done"); // DEBUG
  } else if (recording_filename.length() == 0) {
    // Find last file in directory that will be reused
    Serial.print("Will look for files in "); Serial.println(recording_directory); delay(50); // DEBUG
    String last_filename = find_last_recording_name(recording_directory, false);
    if (last_filename.length() == 0) {
      recording_filename = String("0000.tsv");
    } else {
      recording_filename = last_filename;
    }
  }
  do { // Increment the file name (0001.tsv, 0002.tsv, ...).
    if (recording_filename.startsWith("9999")) {
      Serial.println("Too many files. This program does not go beyond 9999.tsv.");  // NOTE: Will enter an infinite loop
      tft.fillScreen(TFT_BLUE);
      tft.setTextColor(TFT_LIGHTGREY);
      tft.drawString("Too many files. This program does not go beyond 9999.tsv.", 20, 100, 2);  // Using font 2
    } else {
      Serial.println(recording_filename); delay(10); // DEBUG
      for (int p = 3; p >= 0; --p) {
        char c = recording_filename.charAt(p);
        if (c == '9') {
          recording_filename.setCharAt(p, '0'); // Roll over from '9' to '0', loop will continue with preceeding character (digit)
        } else {
          recording_filename.setCharAt(p, c + 1); // Increment and exit the loop
          break;
        }
      }
    }
    recording_path = recording_directory + String("/") + recording_filename;
    // Repeat in case the newly constructed filename actually would exist
  } while (SD.exists(recording_path));

  delay(10); //DEBUG
  recording_file = SD.open(recording_path, FILE_WRITE);
  delay(10); //DEBUG
  if (recording_file) {
    is_recording = true;
    // TODO: Write a file header that describes the quantities (timestamp and chosen scale) and units
    
    digitalWrite(LED_BUILTIN, HIGH); // Turn on the built-in LED when recording
    tft.drawString("Recording started", 20, 100, 4);  // Using font 4
    // Frequency-controlled tone on external speaker connected to the left Grove port
    tone(PIN_WIRE_SCL, 440, 140); delay(140);  // play 440 Hz non-blocking but also wait for it during 140 ms
    tone(PIN_WIRE_SCL, 660, 140);  // play 660 Hz non-blocking for 140 ms
  } else {
    // Error
    tft.fillScreen(TFT_YELLOW);
    tft.setTextColor(TFT_RED);
    tft.drawString("Could not start recording.", 20, 80, 4);  // Using font 4
    tft.drawString("Is an SD card inserted?", 20, 120, 4);  // Using font 4  
    // Play an error sound
    tone(PIN_WIRE_SCL, 280, 70);  // play 280 Hz non-blocking for 70 ms
    delay(70); // wait 70 ms
    tone(PIN_WIRE_SCL, 260, 100);  // play 260 Hz non-blocking for 100 ms
    Serial.print("Failed to crete file!"); 
  }
}

void resetFileZoom() { 
  /* Reset file-info to not keep an old zoom level. */
  file_point_count = 0;
  file_timestamp_start = 0.0;
  file_timestamp_end = 0.0;
  file_zoom_duration = 0.0; // [s] With 0 meaning entire file
  file_zoom_offset = 0.0;
}

void showFile() {
  /* Show a chosen file's contents in a diagram, and handle button actions in this mode. 
  
  The arrow buttons are used to zoom and pan within the file.
  */
  if (is_recording) { // (This check should be redundant, already not possible to come here by menu system.)
    tft.fillScreen(TFT_YELLOW);
    tft.setTextColor(TFT_RED);
    tft.drawString("Can not show file while recording", 20, 100, 2);  // Using font 2
    menu_level == MODE_2_MENU;
    need_to_update_menu_screen = true;
    return;
  }
  
  tft.fillScreen(TFT_CYAN);
  spr.fillSprite(TFT_CYAN); // So that when diagram & sprite are show, old diagrams don't remain
  // Show small hints for the A, B and C buttons
  tft.setTextColor(TFT_LIGHTGREY);
  tft.drawString("delete", 3, -1, 1);  // Using font 1
  tft.drawString("export", 72, -1, 1);  // Using font 1
  tft.drawString("browse", 160, -1, 1);  // Using font 1
  // Show the file path in the header bar
  tft.setTextColor(TFT_BLACK);
  tft.drawString(browser_directory, 2, 7, 2);  // Using font 2
  tft.drawString("/", 48, 7, 2);  // Using font 2
  tft.drawString(browser_cursor_name, 58, 7, 2);  // Using font 2
  
  bool reloading = file_point_count >= 1 && file_zoom_duration > 0.0 && file_timestamp_end > 0.0;
  if (reloading) {
    // Re-loading the same file, to perhaps handle zooming or scrolling.
    // TODO: Consider an Array type instead of queue, so that loaded data
    // can hopefully be reused for updates without re-reading.
    // For now, it seemed simpler (and without memory limit), to just keep track of file length
    // and zoom setting until a different file loaded, and re-read based on scroll-fraction.
    
    // Handle buttons (before updating screen)
    if (button_pressed == WIO_5S_DOWN) {
      // Zoom in along time axis, until some minimum number of samples is shown
      file_zoom_duration /= 2.0;
      file_zoom_duration = max(10.0, file_zoom_duration);  // Don't allow less than 10 seconds of range
      file_zoom_offset += file_zoom_duration / 2.0;
      file_zoom_offset = max(0.0, min(file_zoom_offset, file_timestamp_end - file_timestamp_start - file_zoom_duration));
    } else if (button_pressed == WIO_5S_UP) {
      // Zoom out along time axis, until entire file shown
      file_zoom_offset -= file_zoom_duration / 2.0;
      file_zoom_duration *= 2.0;
      file_zoom_duration = min(file_zoom_duration, file_timestamp_end - file_timestamp_start);
      file_zoom_offset = max(0.0, file_zoom_offset);
    } if (button_pressed == WIO_5S_LEFT) {
      // Scroll along the time axis, if zoomed in
      file_zoom_offset -= file_zoom_duration * 0.6666666667;
      file_zoom_offset = max(0.0, file_zoom_offset);
    } if (button_pressed == WIO_5S_RIGHT) {
      file_zoom_offset += file_zoom_duration * 0.6666666667;
      file_zoom_offset = max(0.0, min(file_zoom_offset, file_timestamp_end - file_timestamp_start - file_zoom_duration));
    } if (button_pressed == WIO_5S_PRESS) {
      // Toggle whether the threshold curve should be shown (same setting as for the live diagram).
      show_threshold = !show_threshold;
      tft.fillScreen(TFT_WHITE);
      tft.setTextColor(TFT_BLACK);
      if (show_threshold) {
        tft.drawString("Showing threshold curve", 10, 150, 4);  // Using font 4
      } else {
        tft.drawString("Hiding threshold curve", 10, 150, 4);  // Using font 4
      }
    }
  }
  
  bool export_to_serial = (button_pressed == WIO_KEY_B);
  if (button_pressed == WIO_KEY_C) {
    menu_level = MODE_7_FILE_DELETE;
    button_pressed = VIRTUAL_BUTTON_PRESS; // Acts as a virtual button press event in the next loop(), to get the browser shown then.
    return;
  }

  File input_file = SD.open(browser_directory + String("/") + browser_cursor_name, FILE_READ);
  if (!input_file) {
    Serial.println("Failed to open file.");
    menu_level = MODE_5_FILE_BROWSER;
    button_pressed = VIRTUAL_BUTTON_PRESS; // Acts as a virtual button press event in the next loop(), to get the browser shown then.
    resetFileZoom();
    return;
  }
  double file_size = double(input_file.size()); // [B]
  snprintf(buffer, BUFFER_LENGTH, "Loading %.1f kB file...", file_size * 0.001);
  tft.drawString(buffer, 20, 40, 2);  // Using font 2
  if (export_to_serial) {
    Serial.print("\n=== Export of \"");
    Serial.print(browser_directory + String("/") + browser_cursor_name);
    Serial.println("\" ===");
    tft.drawString("Exporting to serial over USB...", 20, 55, 2); // Using font 2
  }
  double zoom_fraction_start;
  double zoom_fraction_end;
  long zoom_point_start = 0;
  long zoom_point_end = 0;
  long decimation = 1;
  if (reloading) {
    zoom_fraction_start = file_zoom_offset / (file_timestamp_end - file_timestamp_start);
    zoom_fraction_end = (file_zoom_offset + file_zoom_duration) / (file_timestamp_end - file_timestamp_start);
    zoom_point_start = long(zoom_fraction_start * file_point_count);
    zoom_point_end = long(zoom_fraction_end * file_point_count + 0.999999);
    long zoom_point_count = zoom_point_end - zoom_point_start;
    if (zoom_point_count > 400) {
      // Choose decimation to use (just over) 400 points for the diagram.
      // The SCREEN_WIDTH is 320 pixels, and at least one case of loading 1070 points caused a freeze in content.draw().
      decimation = zoom_point_count / 400;
    }
  } else {
    // First loading, recorded duration is not known.
    zoom_fraction_start = 0.0;
    zoom_fraction_end = 1.0;
    zoom_point_end = LONG_MAX;
    if (file_size > 10E3) {
      // One row of data is approximately 20 bytes. Aim for about 320 points in the diagram 
      decimation = max(2, long((file_size / 20.0) / 320.0));
    }
  }
  if (zoom_fraction_end - zoom_fraction_start < 1.0) {
    snprintf(buffer, BUFFER_LENGTH, "Zoom: %.1f to %.1f of %.1f minutes (%.0f %%).",
      file_zoom_offset / 60.0,
      (file_zoom_offset + file_zoom_duration) / 60.0,
      (file_timestamp_end - file_timestamp_start) / 60.0,
      (zoom_fraction_end - zoom_fraction_start) * 100.0);
    tft.drawString(buffer, 20, 85, 2); // Using font 2
    if (decimation != 1) {
      snprintf(buffer, BUFFER_LENGTH, "Decimation: showing 1 point out of %d.", decimation);
      tft.drawString(buffer, 20, 100, 2); // Using font 2
    }
  }
  
  long input_point_counter = 0;
  long point_counter = 0;
  double first_timestamp = 0.0;
  double last_timestamp = 0.0;
  doubles loaded_data;
  doubles loaded_ref;
  double loaded_min_value = 5E3;
  while (true) { // input_file.available()) {
    // NOTE: I have not seen any indication that there is a double-version of parseFloat(),
    // but converting (likely float) to double as preparation for use in line chart.
    double timestamp = input_file.parseFloat();
    if (input_point_counter == 0) {
      first_timestamp = timestamp;
    }
    if (timestamp == 0.0 || input_point_counter >= zoom_point_end) {
      // parseFloat() return 0.0 when the file ends (input_file.available() might still be true due to whitespace)
      break;
    }
    last_timestamp = timestamp;
    double value = input_file.parseFloat();
    double threshold = input_file.parseFloat();
    
    if (export_to_serial) {
      // Write the same values to the serial port (to computer), without any decimation or zoom.
      snprintf(buffer, BUFFER_LENGTH, "%.2f\t%.2f\t%.1f\n", timestamp, value, threshold);
      Serial.print(buffer);
    }
    
    if (input_point_counter++ >= zoom_point_start) {
      if (point_counter++ % decimation == 0) {
        loaded_min_value = min(loaded_min_value, value);
        loaded_data.push(value);
        if (show_threshold) {
          loaded_ref.push(threshold);
        }
      }
    }
  }
  input_file.close();
  if (point_counter == 0) {
    Serial.println("No data loaded.");
    menu_level = MODE_5_FILE_BROWSER;
    button_pressed = VIRTUAL_BUTTON_PRESS; // Acts as a virtual button press event in the next loop(), to get the browser shown then.
    resetFileZoom();
    return;
  }

  if (!reloading) {
    // Newly opened file. Starts fully zoomed out (autoscaled) to show the entire file.
    file_point_count = input_point_counter;
    file_timestamp_start = first_timestamp;
    file_timestamp_end = last_timestamp;
    file_zoom_duration = file_timestamp_end - file_timestamp_start; // [s] With 0 meaning entire file
    file_zoom_offset = 0.0;
    zoom_fraction_start = file_zoom_offset / (file_timestamp_end - file_timestamp_start);
    zoom_fraction_end = (file_zoom_offset + file_zoom_duration) / (file_timestamp_end - file_timestamp_start);
  }
  if (export_to_serial) {
    Serial.print("=-=-=-=\n");
  }

  line_chart content = line_chart(2, 0); // (left, top) where the line graph begins (y-axis will use some space internally)
  content // The following methods return the object again so they can be chained like .a().b().c()...:
          .tick(2) // This affects t-tick length and x-axis size on display
          .x_skip_tick(0.0f) // DEBUG what does this do? Affects x step size and thus diagram range
          .width(DIAGRAM_WIDTH) // TODO DEBUG: When DIAGRAM_WIDTH is greater than 295 px the latest points are not shown! (As if hidden by white.)
          .height(DIAGRAM_HEIGHT) // Height of the actual line chart
          .based_on(loaded_min_value - 0.01) // Low starting point of y-axis. Subtract something to handle bug of exact min being behind x-axis.
          .show_circle(false); // Disable drawing of points (draw just line)
  if (show_threshold) {
    content.value({loaded_ref, loaded_data}); // Show also the reference value
    content.color(TFT_LIGHTGREY, DIAGRAM_COLOR); // Line colors
  } else {
    content.value(loaded_data); // The data (queue) to show (gets copied to an array by this call, in content._value which is private but accesible )
    content.color(DIAGRAM_COLOR); // Line color
  }
  content // The following methods return the object again so they can be chained like .a().b().c()...:
          .y_role_thickness(2)
          .x_role_color(TFT_LIGHTGREY)  // Color for y-axis
          .x_tick_color(TFT_LIGHTGREY)  // Color for y-axis tick labels
          .y_role_color(TFT_LIGHTGREY)  // Color for y-axis
          .y_tick_color(TFT_LIGHTGREY)  // Color for y-axis tick labels
          // Note: to change the pan_color that seems to control the dotted lines for ticks, seeeded_graphics_define.h would need to be modified.
          .draw();
  spr.pushSprite(0, HEADER_HEIGHT);  // Request the diagram to be shown, at (left, top).
  
  // Print diagnostics for debugging
  Serial.print("Number of datapoints: "); Serial.println(file_point_count);
  Serial.print("Timestamps from "); Serial.print(file_timestamp_start);
  Serial.print(" to "); Serial.println(file_timestamp_end);
  Serial.print("Zoom range from "); Serial.print(file_zoom_offset);
  Serial.print(" to "); Serial.print(file_zoom_offset + file_zoom_duration);
  Serial.print(", duration "); Serial.println(file_zoom_duration);
  // Draw a scrollbar
  tft.drawLine(0, HEADER_HEIGHT - 3, SCREEN_WIDTH, HEADER_HEIGHT - 3, TFT_LIGHTGREY);
  tft.drawLine(0, HEADER_HEIGHT - 2, SCREEN_WIDTH, HEADER_HEIGHT - 2, TFT_LIGHTGREY);
  tft.drawLine(int(SCREEN_WIDTH * zoom_fraction_start), HEADER_HEIGHT - 3,
               int(SCREEN_WIDTH * zoom_fraction_end), HEADER_HEIGHT - 3, DIAGRAM_COLOR);
  tft.drawLine(int(SCREEN_WIDTH * zoom_fraction_start), HEADER_HEIGHT - 2,
               int(SCREEN_WIDTH * zoom_fraction_end), HEADER_HEIGHT - 2, DIAGRAM_COLOR);
  
  // Show file size and zoom & scroll info in the top right corner
  if (decimation == 1) {
    snprintf(buffer, BUFFER_LENGTH, "%d points loaded", point_counter);
  } else {
    snprintf(buffer, BUFFER_LENGTH, "%d / %d points shown", point_counter, decimation);
  }
  tft.drawString(buffer, 140, 7, 1); // Using font 1 (small)
  if (zoom_fraction_start == 0.0 && zoom_fraction_end == 1.0) {  // Fully zoomed out
    snprintf(buffer, BUFFER_LENGTH, "entire %.0f s (%.1f min.) file",
      file_timestamp_end - file_timestamp_start,
      (file_timestamp_end - file_timestamp_start) / 60.0);
  } else if (file_timestamp_end - file_timestamp_start > 999.0) {  // Longer than 999 seconds (16.65 minutes)
    snprintf(buffer, BUFFER_LENGTH, "%.0f to %.0f s of %.0f s",
      file_zoom_offset,
      file_zoom_offset + file_zoom_duration,
      file_timestamp_end - file_timestamp_start);
  } else {
    snprintf(buffer, BUFFER_LENGTH, "%.1f to %.1f min. of %.1f min.",
      file_zoom_offset / 60.0,
      (file_zoom_offset + file_zoom_duration) / 60.0,
      (file_timestamp_end - file_timestamp_start) / 60.0);
  }
  tft.drawString(buffer, 140, 16, 1); // Using font 1 (small)
  
  delay(16);
  // Try harder to clear memory
  while (!loaded_data.empty()) { loaded_data.pop(); }
  while (!loaded_ref.empty()) { loaded_ref.pop(); }
}

void confirmDelete() {
  /* Show a question about deleting the chosen file, and handle button actions in this mode. */
  if (button_pressed == WIO_5S_PRESS) {
    // Clicked joystick to confirm deletion
    String path = browser_directory + "/" + browser_cursor_name;
    SD.remove(path);
    delay(10); // [ms]
    menu_level = MODE_5_FILE_BROWSER;
    browser_cursor_name = String(""); // Will cause create_browser_list() to select the last file.
    create_browser_list();
    button_pressed = VIRTUAL_BUTTON_PRESS; // Acts as a virtual button press event in the next loop(), to get the browser shown then.

  } else if (button_pressed != 0 && button_pressed != VIRTUAL_BUTTON_PRESS) {
    // Any other button cancels and returns to file list
    menu_level = MODE_5_FILE_BROWSER;
    create_browser_list();
    button_pressed = VIRTUAL_BUTTON_PRESS; // Acts as a virtual button press event in the next loop(), to get the browser shown then.

  } else {
    // TODO show the question
    tft.fillScreen(TFT_GREEN);
    tft.setTextColor(TFT_BLACK);
    tft.drawString(browser_directory + "/" + browser_cursor_name, 20, 2, 2);  // Using font 2
    tft.drawString("Delete this file?", 20, 40, 4);  // Using font 4
    tft.drawString("Click central button to delete", 20, 60, 2);  // Using font 2
    tft.drawString("or any other button to cancel.", 20, 80, 2);  // Using font 2
  }
}
