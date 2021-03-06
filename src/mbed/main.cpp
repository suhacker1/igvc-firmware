#include "mbed.h"

#include <cstring>
#include <string>

#include <EthernetInterface.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include "igvc.pb.h"
#include "encoder_pair/encoder_pair.h"
#include "sabertooth_controller/sabertooth_controller.h"
#include "utils.h"

/* hardware definitions */
Timer g_timer;
EncoderPair encoders;
SaberToothController g_motor_controller;

/* mbed pin definitions */
DigitalOut g_mbed_led1(LED1);
DigitalOut g_mbed_led2(LED2);
DigitalOut g_mbed_led3(LED3);
DigitalOut g_mbed_led4(LED4);
DigitalOut g_board_led(p8);
DigitalOut g_safety_light_enable(p11);
DigitalIn g_e_stop_status(p15);
AnalogIn g_battery(p19);

/* PID calculation values */
long g_last_cmd_time = 0;
int g_last_loop_time = 0;
float g_error_l = 0;
float g_error_r = 0;
float g_d_error_l = 0;
float g_d_error_r = 0;
float g_i_error_l = 0;
float g_i_error_r = 0;
float g_d_t_sec = 0;
float g_actual_speed_last_l = 0;
float g_actual_speed_last_r = 0;
float g_low_passed_pv_l = 0;
float g_low_passed_pv_r = 0;

/* Motor Data (see utils.h) */
MotorCoeffs g_motor_coeffs;
MotorStatusPair g_motor_pair;

/* e-stop logic */
int g_estop = 1;

/* function prototypes */
void parseRequest(const RequestMessage &req);
bool sendResponse(TCPSocket &client);
void pid();
void triggerEstop();

int main()
{
  //    /* Read PCON register */
  //  printf("PCON: 0x%x\n", *((unsigned int *)0x400FC180));
  //  *(unsigned int *)0x400fc180 |= 0xf;

  Serial pc(USBTX, USBRX);
  /* Open the server (mbed) via the EthernetInterface class */
  pc.printf("Connecting...\r\n");
  EthernetInterface net;

  if (int ret = net.set_network(MBED_IP, NETMASK, COMPUTER_IP); ret != 0)
  {
    pc.printf("Error performing set_network(). Error code: %i\r\n", ret);
    return 1;
  }
  if (int ret = net.connect(); ret != 0)
  {
    pc.printf("Error performing connect(). Error code: %i\r\n", ret);
    return 1;
  }

  const char *ip = net.get_ip_address();
  pc.printf("MBED's IP address is: %s\n", ip ? ip : "No IP");

  /* Instantiate a TCP Socket to function as the server and bind it to the
   * specified port */
  TCPSocket server_socket;
  if (int ret = server_socket.open(&net); ret != 0)
  {
    pc.printf("Error opening TCPSocket. Error code: %i\r\n", ret);
    return 1;
  }

  if (int ret = server_socket.bind(MBED_IP, SERVER_PORT); ret != 0)
  {
    pc.printf("Error binding TCPSocket. Error code: %i\r\n", ret);
    return 1;
  }

  if (int ret = server_socket.listen(1); ret != 0)
  {
    pc.printf("Error listening. Error code: %i\r\n", ret);
    return 1;
  }

  g_timer.reset();
  g_timer.start();

  /* Instantiage a TCP socket to serve as the client */
  TCPSocket *client = nullptr;

  while (true)
  {
    g_mbed_led2 = 1;
    /* wait for a new TCP Connection */
    pc.printf("Waiting for new connection...\r\n");
    client = server_socket.accept();
    g_mbed_led2 = 0;

    SocketAddress socket_address;
    client->getpeername(&socket_address);
    pc.printf("Accepted client from %s\r\n", socket_address.get_ip_address());

    g_estop = 1;

    while (true)
    {
      /* read data into the buffer. This call blocks until data is read */
      char buffer[BUFFER_SIZE];
      int n = client->recv(buffer, sizeof(buffer) - 1);

      /*
      n represents the response message for the read() command.
      - if n == 0 then the client closed the connection
      - otherwise, n is the number of bytes read
      */
      if (n < 0)
      {
        if (DEBUG)
        {
          printf("Received empty buffer\n");
        }
        wait_ms(10);
        continue;
      }
      if (n == 0)
      {
        pc.printf("Client Closed Connection\n");
        break;
      }
      if (DEBUG)
      {
        printf("Received Request of size: %d\n", n);
      }


      /* protobuf message to hold request from client */
      RequestMessage request = RequestMessage_init_zero;
      bool istatus;

      /* Create a stream that reads from the buffer. */
      pb_istream_t istream = pb_istream_from_buffer(reinterpret_cast<uint8_t *>(buffer), n);

      /* decode the message */
      istatus = pb_decode(&istream, RequestMessage_fields, &request);

      /* check for any errors.. */
      if (!istatus)
      {
        printf("Decoding failed: %s\n", PB_GET_ERROR(&istream));
        continue;
      }

      parseRequest(request);

      /* reset the timer */
      if (g_timer.read_ms() > pow(2, 20))
      {
        g_timer.reset();
        g_last_cmd_time = 0;
      }

      /* e-stop logic */
      if (g_e_stop_status.read() == 0)
      {
        triggerEstop();
      }
      else
      {
        g_estop = 1;
        g_safety_light_enable = 0;
      }

      /* update motor velocities with PID */
      pid();

      if (!sendResponse(*client))
      {
        printf("Couldn't send response to client!\r\n");
        continue;
      }
    }
    pc.printf("Closing rip..\r\n");
    triggerEstop();
    client->close();
  }
}

bool sendResponse(TCPSocket &client)
{
  /* protocol buffer to hold response message */
  ResponseMessage response = ResponseMessage_init_zero;

  /* This is the buffer where we will store the response message. */
  uint8_t responsebuffer[256];
  size_t response_length;
  bool ostatus;

  /* Create a stream that will write to our buffer. */
  pb_ostream_t ostream = pb_ostream_from_buffer(responsebuffer, sizeof(responsebuffer));

  /* Fill in the message fields */
  response.has_p_l = true;
  response.has_p_r = true;
  response.has_i_l = true;
  response.has_i_r = true;
  response.has_d_l = true;
  response.has_d_r = true;
  response.has_speed_l = true;
  response.has_speed_r = true;
  response.has_dt_sec = true;
  response.has_voltage = true;
  response.has_estop = true;

  response.has_kv_l = true;
  response.has_kv_r = true;

  response.has_left_output = true;
  response.has_right_output = true;

  response.p_l = static_cast<float>(g_motor_coeffs.left.k_p);
  response.p_r = static_cast<float>(g_motor_coeffs.right.k_p);
  response.i_l = static_cast<float>(g_motor_coeffs.left.k_i);
  response.i_r = static_cast<float>(g_motor_coeffs.right.k_i);
  response.d_l = static_cast<float>(g_motor_coeffs.left.k_d);
  response.d_r = static_cast<float>(g_motor_coeffs.right.k_d);

  response.speed_l = static_cast<float>(g_motor_pair.left.actual_speed);
  response.speed_r = static_cast<float>(g_motor_pair.right.actual_speed);
  response.dt_sec = static_cast<float>(g_d_t_sec);
  response.voltage = static_cast<float>(g_battery.read() * 3.3 * 521 / 51);
  response.estop = static_cast<bool>(g_estop);

  response.kv_l = static_cast<float>(g_motor_coeffs.left.k_kv);
  response.kv_r = static_cast<float>(g_motor_coeffs.right.k_kv);

  response.left_output = g_motor_pair.left.ctrl_output;
  response.right_output = g_motor_pair.right.ctrl_output;

  /* encode the message */
  ostatus = pb_encode(&ostream, ResponseMessage_fields, &response);
  response_length = ostream.bytes_written;

  if (DEBUG)
  {
    printf("Sending message of length: %zu\n", response_length);
  }

  /* Then just check for any errors.. */
  if (!ostatus)
  {
    printf("Encoding failed: %s\n", PB_GET_ERROR(&ostream));
    return false;
  }

  client.send(reinterpret_cast<char *>(responsebuffer), response_length);
  return true;
}

void triggerEstop()
{
  // If get 5V, since inverted, meaning disabled on motors
  g_estop = 0;
  g_motor_pair.left.desired_speed = 0;
  g_motor_pair.right.desired_speed = 0;
  g_i_error_l = 0;
  g_i_error_r = 0;
  g_motor_controller.stopMotors();
  g_safety_light_enable = 1;
}

/*
Update global variables using most recent client request.
@param[in] req RequestMessage protobuf with desired values
*/
void parseRequest(const RequestMessage &req)
{
  /* request contains PID values */
  if (req.has_p_l)
  {
    g_motor_coeffs.left.k_p = req.p_l;
    g_motor_coeffs.right.k_p = req.p_r;
    g_motor_coeffs.left.k_d = req.d_l;
    g_motor_coeffs.right.k_d = req.d_r;
    g_motor_coeffs.left.k_i = req.i_l;
    g_motor_coeffs.right.k_i = req.i_r;
    g_motor_coeffs.left.k_kv = req.kv_l;
    g_motor_coeffs.right.k_kv = req.kv_r;
  }
  /* request contains motor velocities */
  if (req.has_speed_l)
  {
    g_motor_pair.left.desired_speed = req.speed_l;
    g_motor_pair.right.desired_speed = req.speed_r;
  }
}

// https://en.wikipedia.org/wiki/PID_controller#Discrete_implementation but with
// e(t) on velocity, not position Changes to before 1: Derivative on PV 2:
// Corrected integral 3: Low pass on Derivative 4: Clamping on Integral 5: Feed
// forward
void pid()
{
  // 1: Calculate dt
  g_d_t_sec = static_cast<float>(g_timer.read_ms() - g_last_loop_time) / 1000.0;

  if (g_timer.read() >= 1700)
  {
    g_timer.reset();
    g_last_loop_time = 0;
  }

  g_last_loop_time = g_timer.read_ms();

  // 2: Convert encoder values into velocity
  g_motor_pair.left.actual_speed = (METERS_PER_TICK * encoders.getLeftTicks()) / g_d_t_sec;
  g_motor_pair.right.actual_speed = (METERS_PER_TICK * encoders.getRightTicks()) / g_d_t_sec;

  // 3: Calculate error
  g_error_l = g_motor_pair.left.desired_speed - g_motor_pair.left.actual_speed;
  g_error_r = g_motor_pair.right.desired_speed - g_motor_pair.right.actual_speed;

  // 4: Calculate Derivative Error
  // TODO(oswinso): Make alpha a parameter
  float alpha = 0.75;
  g_low_passed_pv_l = alpha * (g_actual_speed_last_l - g_motor_pair.left.actual_speed) / g_d_t_sec + (1 - alpha) * g_low_passed_pv_l;
  g_low_passed_pv_r = alpha * (g_actual_speed_last_r - g_motor_pair.right.actual_speed) / g_d_t_sec + (1 - alpha) * g_low_passed_pv_r;

  g_d_error_l = g_low_passed_pv_l;
  g_d_error_r = g_low_passed_pv_r;

  // 5: Calculate Integral Error
  // 5a: Calculate Error
  g_i_error_l += g_error_l * g_d_t_sec;
  g_i_error_r += g_error_r * g_d_t_sec;

  // 5b: Perform clamping
  // TODO(oswinso): make clamping a parameter
  float i_clamp = 60 / g_motor_coeffs.left.k_i;
  g_i_error_l = min(i_clamp, max(-i_clamp, g_i_error_l));
  g_i_error_r = min(i_clamp, max(-i_clamp, g_i_error_r));

  // 6: Sum P, I and D terms
  float feedback_left = g_motor_coeffs.left.k_p * g_error_l + g_motor_coeffs.left.k_d * g_d_error_l + g_motor_coeffs.left.k_i * g_i_error_l;
  float feedback_right = g_motor_coeffs.right.k_p * g_error_r + g_motor_coeffs.right.k_d * g_d_error_r + g_motor_coeffs.right.k_i * g_i_error_r;

  // 7: Calculate feedforward
  float feedforward_left = g_motor_coeffs.left.k_kv * g_motor_pair.left.desired_speed;
  float feedforward_right = g_motor_coeffs.right.k_kv * g_motor_pair.right.desired_speed;

  int left_signal = static_cast<int>(round(feedforward_left + feedback_left));
  int right_signal = static_cast<int>(round(feedforward_right + feedback_right));

  // 8: Deadband
  if (abs(g_motor_pair.left.actual_speed) < 0.16 && abs(g_motor_pair.left.desired_speed) < 0.16)
  {
    left_signal = 0;
  }

  if (abs(g_motor_pair.right.actual_speed) < 0.16 && abs(g_motor_pair.right.desired_speed) < 0.16)
  {
    right_signal = 0;
  }

  g_motor_controller.setSpeeds(right_signal, left_signal);

  g_motor_pair.left.ctrl_output = g_motor_controller.getLeftOutput();
  g_motor_pair.right.ctrl_output = g_motor_controller.getRightOutput();

  g_actual_speed_last_l = g_motor_pair.left.actual_speed;
  g_actual_speed_last_r = g_motor_pair.right.actual_speed;
}
