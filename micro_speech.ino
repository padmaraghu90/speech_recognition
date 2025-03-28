/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <TensorFlowLite.h>

#include "audio_provider.h"
#include "command_responder.h"
#include "feature_provider.h"
#include "main_functions.h"
#include "micro_features_micro_model_settings.h"
#include "micro_features_model.h"
#include "recognize_commands.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/system_setup.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include <ArduinoBLE.h>

#define MODE_INFERENCE 0
#define MODE_TRAINING  1

int current_mode = MODE_INFERENCE; // Default mode

#undef PROFILE_MICRO_SPEECH

// Globals, used for compatibility with Arduino-style sketches.
namespace {
tflite::ErrorReporter* error_reporter = nullptr;
const tflite::Model* model = nullptr;
tflite::MicroInterpreter* interpreter = nullptr;
TfLiteTensor* model_input = nullptr;
FeatureProvider* feature_provider = nullptr;
RecognizeCommands* recognizer = nullptr;
int32_t previous_time = 0;

// Create an area of memory to use for input, output, and intermediate arrays.
// The size of this will depend on the model you're using, and may need to be
// determined by experimentation.
constexpr int kTensorArenaSize = 10 * 1024;
uint8_t tensor_arena[kTensorArenaSize];
int8_t feature_buffer[kFeatureElementCount];
int8_t* model_input_buffer = nullptr;
}  // namespace

constexpr int feature_points_byte_count = 4;
constexpr int feature_struct_byte_count =
    (sizeof(int32_t)) + feature_points_byte_count;

#define BLE_SENSE_UUID(val) ("4798e0f2-" val "-4d68-af64-8a8f5258404e")
BLEService service(BLE_SENSE_UUID("0000"));
BLECharacteristic featureCharacteristic(BLE_SENSE_UUID("300a"), BLERead,
                                       feature_struct_byte_count);

uint8_t feature_struct_buffer[feature_struct_byte_count] = {};
int32_t* feature_transmit_length =
    reinterpret_cast<int32_t*>(feature_struct_buffer);
int8_t* feature_points =
    reinterpret_cast<int8_t*>(feature_struct_buffer + (sizeof(int32_t)));

bool training_in_progress = false;

void clearSerialBuffer() {
    while (Serial.available() > 0) {
        Serial.read(); // Read and discard any leftover characters
    }
}
void selectMode() {
    Serial.println("Select mode:");
    Serial.println("1. Inference");
    Serial.println("2. Training");

    // Wait for user input
    while (!Serial.available());

    // Read the input
    int mode = Serial.parseInt();

    // Set the current mode
    if (mode == 1) {
        current_mode = MODE_INFERENCE;
        Serial.println("Selected mode: Inference");
    } else if (mode == 2) {
        current_mode = MODE_TRAINING;
        Serial.println("Selected mode: Training");
    } else {
        Serial.println("Invalid input. Defaulting to Inference.");
        current_mode = MODE_INFERENCE;
    }

    clearSerialBuffer();
}

int32_t getTargetClassFromUser() {

    Serial.println("Enter yes or no");

    // Wait for user input
    while (!Serial.available()); // Wait until data is available

    // Read the input string
    String input = Serial.readString();
    input.trim(); // Remove any leading/trailing whitespace

    // Convert input to lowercase for case-insensitive comparison
    input.toLowerCase();

    // Compare input to "yes" or "no" and return the target class index
    if (input == "silence") {
        TF_LITE_REPORT_ERROR(error_reporter, "User input: silence (0)");
        return 0; // "yes" corresponds to class index 1
    } else if (input == "unknown") {
        TF_LITE_REPORT_ERROR(error_reporter, "User input: unknown (1)");
        return 1; // "no" corresponds to class index 0
    } else if (input == "yes") {
        TF_LITE_REPORT_ERROR(error_reporter, "User input: yes (2)");
        return 2; // "no" corresponds to class index 0
    } else if (input == "no") {
        TF_LITE_REPORT_ERROR(error_reporter, "User input: no (3)");
        return 3; // "no" corresponds to class index 0
    } else {
        TF_LITE_REPORT_ERROR(error_reporter, "Invalid input. Please enter 'yes' or 'no'.");
        return -1; // Invalid input
    }
} 


#define NUM_CLASSES 4
#define NUM_PARAMETERS 16652

// Compute pseudo-updates
float* computePseudoUpdates(TfLiteTensor* output, int32_t target_class) {
  uint8_t* output_data = output->data.uint8;

  // Dequantize the output
  float output_scale = output->params.scale;
  int output_zero_point = output->params.zero_point;
  float dequantized_output[NUM_CLASSES]; // Replace with the actual number of classes
  for (int i = 0; i < NUM_CLASSES; i++) {
      dequantized_output[i] = (output_data[i] - output_zero_point) * output_scale;
  }

  // Compute pseudo-updates using the dequantized output

    float* updates = new float[NUM_PARAMETERS];
    for (int i = 0; i < NUM_PARAMETERS; i++) {
        // Simplified update rule: nudge weights based on prediction error
        updates[i] = dequantized_output[target_class] - output_data[i];
    }
    return updates;
}

#if 0
void sendUpdatesOverBLE(float* updates, int num_updates) {
    int num_chunks = (num_updates * sizeof(float)) / MTU_SIZE;
    if ((num_updates * sizeof(float)) % MTU_SIZE != 0) {
        num_chunks++;
    }

    for (int i = 0; i < num_chunks; i++) {
        int chunk_size = MTU_SIZE;
        if (i == num_chunks - 1) {
            chunk_size = (num_updates * sizeof(float)) % MTU_SIZE;
        }

        // Send the chunk
        pCharacteristic->setValue((uint8_t*)(updates + (i * MTU_SIZE / sizeof(float))), chunk_size);
        pCharacteristic->notify();
        delay(10); // Add a small delay to avoid overwhelming the BLE stack
    }
}
#endif 

void ble_setup() {
  String name;

  if (!BLE.begin()) {
    TF_LITE_REPORT_ERROR(error_reporter, "Failed to initialized BLE!");
    while (true) {
      // NORETURN
    }
  }

  String address = BLE.address();

  TF_LITE_REPORT_ERROR(error_reporter, "address = %s", address.c_str());

  address.toUpperCase();

  name = "BLESense-";
  name += address[address.length() - 5];
  name += address[address.length() - 4];
  name += address[address.length() - 2];
  name += address[address.length() - 1];

  TF_LITE_REPORT_ERROR(error_reporter, "name = %s", name.c_str());

  BLE.setLocalName(name.c_str());
  BLE.setDeviceName(name.c_str());
  BLE.setAdvertisedService(service);

  service.addCharacteristic(featureCharacteristic);

  BLE.addService(service);

  BLE.advertise();

}


// The name of this function is important for Arduino compatibility.
void setup() {

  tflite::InitializeTarget();
  
  Serial.begin(115200);
  while (!Serial); // Wait for Serial to be ready

  // Log a message using Serial
  Serial.println("Initializing Audio recognition example using TensorFlow Lite...");


  selectMode();

  // Set up logging. Google style is to avoid globals or statics because of
  // lifetime uncertainty, but since this has a trivial destructor it's okay.
  // NOLINTNEXTLINE(runtime-global-variables)
  static tflite::MicroErrorReporter micro_error_reporter;
  error_reporter = &micro_error_reporter;


  TF_LITE_REPORT_ERROR(error_reporter, "init done");

  ble_setup();

  // Map the model into a usable data structure. This doesn't involve any
  // copying or parsing, it's a very lightweight operation.
  model = tflite::GetModel(g_model);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    TF_LITE_REPORT_ERROR(error_reporter,
                         "Model provided is schema version %d not equal "
                         "to supported version %d.",
                         model->version(), TFLITE_SCHEMA_VERSION);
    return;
  }

  // Pull in only the operation implementations we need.
  // This relies on a complete list of all the ops needed by this graph.
  // An easier approach is to just use the AllOpsResolver, but this will
  // incur some penalty in code space for op implementations that are not
  // needed by this graph.
  //
  // tflite::AllOpsResolver resolver;
  // NOLINTNEXTLINE(runtime-global-variables)
  static tflite::MicroMutableOpResolver<4> micro_op_resolver(error_reporter);
  if (micro_op_resolver.AddDepthwiseConv2D() != kTfLiteOk) {
    return;
  }
  if (micro_op_resolver.AddFullyConnected() != kTfLiteOk) {
    return;
  }
  if (micro_op_resolver.AddSoftmax() != kTfLiteOk) {
    return;
  }
  if (micro_op_resolver.AddReshape() != kTfLiteOk) {
    return;
  }

  // Build an interpreter to run the model with.
  static tflite::MicroInterpreter static_interpreter(
      model, micro_op_resolver, tensor_arena, kTensorArenaSize, error_reporter);
  interpreter = &static_interpreter;

  // Allocate memory from the tensor_arena for the model's tensors.
  TfLiteStatus allocate_status = interpreter->AllocateTensors();
  if (allocate_status != kTfLiteOk) {
    TF_LITE_REPORT_ERROR(error_reporter, "AllocateTensors() failed");
    return;
  }

  // Get information about the memory area to use for the model's input.
  model_input = interpreter->input(0);
  if ((model_input->dims->size != 2) || (model_input->dims->data[0] != 1) ||
      (model_input->dims->data[1] !=
       (kFeatureSliceCount * kFeatureSliceSize)) ||
      (model_input->type != kTfLiteInt8)) {
    TF_LITE_REPORT_ERROR(error_reporter,
                         "Bad input tensor parameters in model");
    return;
  }
  model_input_buffer = model_input->data.int8;

  // Prepare to access the audio spectrograms from a microphone or other source
  // that will provide the inputs to the neural network.
  // NOLINTNEXTLINE(runtime-global-variables)
  static FeatureProvider static_feature_provider(kFeatureElementCount,
                                                 feature_buffer);
  feature_provider = &static_feature_provider;

  static RecognizeCommands static_recognizer(error_reporter);
  recognizer = &static_recognizer;

  previous_time = 0;

  if (current_mode == MODE_INFERENCE) {
    // start the audio
    TfLiteStatus init_status = InitAudioRecording(error_reporter);
    if (init_status != kTfLiteOk) {
      TF_LITE_REPORT_ERROR(error_reporter, "Unable to initialize audio");
      return;
    }

    TF_LITE_REPORT_ERROR(error_reporter, "Initialization complete");
  } else if (current_mode == MODE_TRAINING) {
    setupRecording();
  }
  
  Serial.println("Setup done!");
}

bool recording_started = false;

void trainingMode()
{
  int32_t current_time;
  const int kMaxSlices = 49; // Example: Buffer can store 49 slices
  int totalSlicesCaptured = 0; // Counter for slices captured

  if (training_in_progress == true)
    return;

  training_in_progress = true;

  int32_t target_class = getTargetClassFromUser();
  if (target_class == -1)
    return;
  
  TF_LITE_REPORT_ERROR(error_reporter, "Start saying the word !");

  startRecording();

  recording_started = true;
}


// The name of this function is important for Arduino compatibility.
void loop() {
#ifdef PROFILE_MICRO_SPEECH
  const uint32_t prof_start = millis();
  static uint32_t prof_count = 0;
  static uint32_t prof_sum = 0;
  static uint32_t prof_min = std::numeric_limits<uint32_t>::max();
  static uint32_t prof_max = 0;
#endif  // PROFILE_MICRO_SPEECH

  if (current_mode == MODE_TRAINING) {
    if (recording_started == false) {
      trainingMode();
      return;     
    }
  } 

  BLEDevice central = BLE.central();

  // if a central is connected to the peripheral:
  static bool was_connected_last = false;
  if (central && !was_connected_last) {
    // print the central's BT address:
    TF_LITE_REPORT_ERROR(error_reporter, "Connected to central: %s",
                         central.address().c_str());
  }
  was_connected_last = central;


  // Fetch the spectrogram for the current time.
  const int32_t current_time = LatestAudioTimestamp();
  int how_many_new_slices = 0;
  TfLiteStatus feature_status = feature_provider->PopulateFeatureData(
      error_reporter, previous_time, current_time, &how_many_new_slices);
  if (feature_status != kTfLiteOk) {
    TF_LITE_REPORT_ERROR(error_reporter, "Feature generation failed");
    return;
  }
  previous_time += how_many_new_slices * kFeatureSliceStrideMs;
  // If no new audio samples have been received since last time, don't bother
  // running the network model.
  if (how_many_new_slices == 0) {
    return;
  }

  // Copy feature buffer to input tensor
  for (int i = 0; i < kFeatureElementCount; i++) {
    model_input_buffer[i] = feature_buffer[i];
  }

  // Run the model on the spectrogram input and make sure it succeeds.
  TfLiteStatus invoke_status = interpreter->Invoke();
  if (invoke_status != kTfLiteOk) {
    TF_LITE_REPORT_ERROR(error_reporter, "Invoke failed");
    return;
  }

  // Obtain a pointer to the output tensor
  TfLiteTensor* output = interpreter->output(0);


  // Inference mode: Just print the results
  // Determine whether a command was recognized based on the output of inference
  const char* found_command = nullptr;
  uint8_t score = 0;
  bool is_new_command = false;
  TfLiteStatus process_status = recognizer->ProcessLatestResults(
      output, current_time, &found_command, &score, &is_new_command);
  if (process_status != kTfLiteOk) {
    TF_LITE_REPORT_ERROR(error_reporter,
                        "RecognizeCommands::ProcessLatestResults() failed");
    return;
  }
  // Do something based on the recognized command. The default implementation
  // just prints to the error console, but you should replace this with your
  // own function for a real application.
  RespondToCommand(error_reporter, current_time, found_command, score,
                  is_new_command);

  if ((current_mode == MODE_TRAINING) && is_new_command) {
      stopRecording();
      Serial.println("Audio captured and features computed.");
      recording_started = false;
      training_in_progress = false;
      if (central && central.connected()) {
        *feature_transmit_length = 4;
        featureCharacteristic.writeValue(feature_struct_buffer,
                                        feature_struct_byte_count);
    }
  }


#ifdef PROFILE_MICRO_SPEECH
  const uint32_t prof_end = millis();
  if (++prof_count > 10) {
    uint32_t elapsed = prof_end - prof_start;
    prof_sum += elapsed;
    if (elapsed < prof_min) {
      prof_min = elapsed;
    }
    if (elapsed > prof_max) {
      prof_max = elapsed;
    }
    if (prof_count % 300 == 0) {
      TF_LITE_REPORT_ERROR(error_reporter,
                           "## time: min %dms  max %dms  avg %dms", prof_min,
                           prof_max, prof_sum / prof_count);
    }
  }
#endif  // PROFILE_MICRO_SPEECH
}
