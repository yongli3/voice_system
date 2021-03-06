/*
* 语音听写(iFly Auto Transform)技术能够实时地将语音转换成对应的文字。
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ros/ros.h>
#include <ros/console.h>
#include <ros/assert.h>

#include <std_msgs/Float32MultiArray.h>
#include <std_msgs/String.h>
#include <std_msgs/Int8.h>
#include <std_msgs/Int32.h>
#include <geometry_msgs/Twist.h>
#include <kobuki_msgs/Led.h>
#include <kobuki_msgs/Sound.h>

#include "qisr.h"
#include "msp_cmn.h"
#include "msp_errors.h"
#include "speech_recognizer.h"
#include "voice_system/TTSService.h"
#include "demo_od/ObjectDetect.h"


enum state_codes { entry, foo, bar, end, fail_code};
enum ret_codes { ok, fail, repeat};

#define EXIT_STATE end
#define ENTRY_STATE entry
#define FOO_STATE foo
#define BAR_STATE bar

#define CODE_OK  ok
#define CODE_FAIL fail
#define CODE_REPEAT repeat

struct transition {
    enum state_codes src_state;
    enum ret_codes   ret_code;
    enum state_codes dst_state;
};
/* transitions from end state aren't needed */
struct transition state_transitions[] = {
    {entry, ok,     foo},
    {entry, fail,   end},
    {foo,   ok,     bar},
    {foo,   fail,   end},
    {foo,   repeat, foo},
    {bar,   ok,     end},
    {bar,   fail,   end},
    {bar,   repeat, foo}};

typedef enum ret_codes FUNC(void);

static enum ret_codes entry_state(void)
{
    printf("+%s\n", __func__);
    return CODE_OK;
}
static enum ret_codes foo_state(void)
{
    printf("+%s\n", __func__);
    return CODE_OK;
}
static enum ret_codes bar_state(void)
{
    printf("+%s\n", __func__);
    return CODE_OK;
}
static enum ret_codes exit_state(void)
{
    printf("+%s\n", __func__);
}

/* array and enum below must be in sync! */
FUNC *state[] = {entry_state, foo_state, bar_state, exit_state};

char *includeStr(char s[], char t[]) {
    int i,j,k;
    for(i = 0; s[i]; ++i) {
        if(s[i] == t[0]) {
            k = i + 1;
            for(j = 1; t[j] == s[k] && s[k] && t[j]; ++k,++j);
            if(t[j] == '\0') return s + i;
        }
    }
    return 0;
}
 
void delStr(char s[], char t[]) {
    char *p;
    while(p = includeStr(s,t))
        strcpy(p,p + strlen(t));
}

// return dst_state
static enum state_codes lookup_transitions(enum state_codes cur_state, enum ret_codes rc) 
{
    int i, len;
    printf("%s %d-%d\n", __func__, cur_state, rc);
    len = sizeof(state_transitions) / sizeof(state_transitions[0]);

    for (i = 0; i < len; i++) {
        if ((state_transitions[i].src_state == cur_state) 
            && (state_transitions[i].ret_code == rc)) {
            return state_transitions[i].dst_state;
        }
    }

    return fail_code;
}

int test_sm() {
    enum state_codes cur_state = ENTRY_STATE;
    enum ret_codes rc = CODE_OK;
    enum ret_codes (* state_fun)(void);

    for (;;) {
        printf("cur_state=%d rc=%d\n", cur_state, rc);
        state_fun = state[cur_state];
        rc = state_fun();
        if (EXIT_STATE == cur_state)
            break;
        cur_state = lookup_transitions(cur_state, rc);
		if (cur_state == fail_code) {
			printf("lookup fail!\n");
			break;
		}
    }

    return EXIT_SUCCESS;
}


// local test
//#define OFFLINE_TEST

#define FRAME_LEN	640 
#define	BUFFER_SIZE	4096

#define ASRSTART        1
#define SYS_UNAUTH		0
#define SYS_AUTH		1

// robot current status
#define CURRENT_IDLE   0
#define CURRENT_PF   	1
#define CURRENT_VSLAM   2
#define CURRENT_FR   	3
#define CURRENT_MOVING 	4
#define CURRENT_OR 		5
#define CURRENT_HAND_OPEN 		6
#define CURRENT_HAND_CLOSE 		7

#define CURRENT_PF_ERROR 50
#define CURRENT_VSLAM_ERROR 51
#define CURRENT_OR_ERROR 52
#define CURRENT_ARM_ERROR 53

using namespace std;
//static string result;

static int voice_manual_code = -1;
// sys status -1=undef, 0=sys unlock, 1=sys locked
static int sys_locked = -1;
static bool speech_end = false;
static bool playing = false;
static int asr_flag = 0;
static char *g_result = NULL;
static unsigned int g_buffersize = BUFFER_SIZE;
static int current_sm = CURRENT_IDLE;

struct st_command {
	char command[255];
	unsigned int code;
};

struct st_object_table {
	char name1[255];
	char name2[255];
};

static struct st_command voice_commands[255];

static struct st_object_table  objects[] = {
	{"瓶子", "bottle"}, {"背包", "bag"}, 
	{"玩具", "toys"}, {"水杯", "cup"}, {"枕头", "pillow"},
	{"椅子", "chair"}, {"显示器", "monitor"}
};

static void show_result(char *str, char is_over)
{
	int i = 0;
	int len = strlen(str);

	printf("+%s [%s]\n", __func__, str);

	for (i = 0; i < len; i++) {
		//printf("%x:", str[i]);
	}
	printf("\n");

	ROS_INFO("+%s [%s]", __func__, str);
	//printf("\rResult: [%s]", str);
	if(is_over)
		putchar('\n');

	//string s(str);
	//result = s;
	asr_flag = 1;
}

static void on_result(const char *result, char is_last)
{
	printf("+%s [%s]\n", __func__, result);
	ROS_INFO("+%s result=%p is_last=%d", __func__, result, is_last);
	
	if (result) {
		size_t left = g_buffersize - 1 - strlen(g_result);
		size_t size = strlen(result);
		if (left < size) {
			g_result = (char*)realloc(g_result, g_buffersize + BUFFER_SIZE);
			if (g_result)
				g_buffersize += BUFFER_SIZE;
			else {
				printf("mem alloc failed\n");
				return;
			}
		}
		strncat(g_result, result, size);
		if (is_last) {
			show_result(g_result, is_last);
		}
	}
}

static void on_speech_begin()
{
	ROS_INFO("+%s g_result=%p", __func__, g_result);
	if (g_result) {
		free(g_result);
		g_result = NULL;
	}
	g_result = (char*)malloc(BUFFER_SIZE);
	g_buffersize = BUFFER_SIZE;
	memset(g_result, 0, g_buffersize);

	speech_end = false;
	ROS_INFO("-%s g_result=%p\n", __func__, g_result);
}

static void on_speech_end(int reason)
{
	ROS_INFO("+%s %d\n", __func__, reason);
	if (reason == END_REASON_VAD_DETECT) {
		ROS_INFO("Speaking done \n");
	}
	else {
		ROS_ERROR("Recognizer error: %d\n", reason);
	}
	speech_end = true;
	
	ROS_INFO("-%s %d\n", __func__, reason);
}

/* demo recognize the audio from microphone */
static void demo_mic(const char* session_begin_params)
{
	int errcode;
	int i = 0;

	struct speech_rec iat;

	struct speech_rec_notifier recnotifier = {
		on_result,
		on_speech_begin,
		on_speech_end
	};

	ROS_INFO("+%s [%s]", __func__, session_begin_params);

	errcode = sr_init(&iat, session_begin_params, SR_MIC, &recnotifier);
	if (errcode) {
		ROS_ERROR("speech recognizer init failed\n");
		return;
	}
	errcode = sr_start_listening(&iat);
	if (errcode) {
		ROS_ERROR("start listen failed %d\n", errcode);
	}
	/* wait for recording end*/
	while(!speech_end) {
		//printf("recflag %d\n", recflag);
		sleep(1);
	}
	errcode = sr_stop_listening(&iat);
	if (errcode) {
		ROS_ERROR("stop listening failed %d\n", errcode);
	}

	sr_uninit(&iat);
	ROS_INFO("-%s", __func__);
}
	 
#define TTS_TEXT(_text) \
 do { \
	memset(tts_content, 0, sizeof(tts_content)); \
	strcat(tts_content, _text); \
	srv.request.target = tts_content; \
	printf("TTS->[%s]\n", _text);\
	if (client.call(srv)) { \
	ROS_INFO("call TTS service okay"); \
	} else { \
		ROS_INFO("call TTS service fail"); \
	} \
 } while (0) 
 
static void asrProcess()
#ifdef OFFLINE_TEST
{
	ROS_INFO("+****%s fake g_result=%p\n", __func__, g_result);
	if (g_result) {
		free(g_result);
		g_result = NULL;
	}
	g_result = (char*)malloc(BUFFER_SIZE);
	g_buffersize = BUFFER_SIZE;
	memset(g_result, 0, g_buffersize);

	strcpy(g_result, "机器人寻找玩具");
	//strcpy(g_result, "机器人前进");
	speech_end = false;
	speech_end = true;
	asr_flag = 1;
	sleep(5);
}
#else
{
	int ret = MSP_SUCCESS;
	/* login params, please do keep the appid correct 58d77a1a*/
	const char* login_params = "appid = 58e631a9, work_dir = .";

	/*
	* See "iFlytek MSC Reference Manual"
	*/
	const char* session_begin_params =
		"sub = iat, domain = iat, language = zh_cn, "
		"accent = mandarin, sample_rate = 16000, "
		"result_type = plain, result_encoding = utf8";

	ROS_INFO("+******%s g_result=%p", __func__, g_result);
	asr_flag = 0;
	if (g_result) {
		free(g_result);
		g_result = NULL;
	}

	// If there is anything is playing, skip the asr

	ROS_INFO("playing=%d", playing);
	if (playing) {
		ROS_INFO("playing skip ...");
		return;
	}

	/* Login first. the 1st arg is username, the 2nd arg is password
	 * just set them as NULL. the 3rd arg is login paramertes 
	 * */
	ret = MSPLogin(NULL, NULL, login_params);
	if (MSP_SUCCESS != ret)	{
		printf("MSPLogin failed , Error code %d.\n",ret);
		goto exit; // login fail, exit the program
	}

	ROS_INFO("Recognizing the speech from microphone");

	demo_mic(session_begin_params);

exit:
	ROS_INFO("-%s g_result=%p", __func__, g_result);
	MSPLogout(); // Logout...

}
#endif

#if 0
static void asrCallback(const std_msgs::Int32::ConstPtr& msg)
{
	ROS_INFO("+%s %d", __func__, msg->data);
     //std::cout<<"Now revoke record asrCallback .. " << endl; 
     
     if (msg->data == ASRSTART) {
        asrProcess(); 
     }
	 ROS_INFO("-%s", __func__);
}
#endif

static void manualCallback(const std_msgs::Int32::ConstPtr& msg)
{
	ROS_INFO("+%s msg=%d", __func__, msg->data);
	voice_manual_code = msg->data;	
}

static void faceCallback(const std_msgs::Int8::ConstPtr& msg)
{
	ROS_INFO("+%s msg=%d sys_locked=%d", __func__, msg->data, sys_locked);

	if (msg->data == SYS_AUTH) {
		sys_locked = 0;
	} else {
		sys_locked = 1;
	}

	ROS_INFO("-%s sys_locked=%d", __func__, sys_locked);
}

static void ttsplayCallback(const std_msgs::Int32::ConstPtr& msg)
{
	ROS_INFO("+%s %d", __func__, msg->data);
	if (msg->data == 1) {
		playing = true;
	} else {
		playing = false;
	}
}

static void resultsCallback(const std_msgs::Int32::ConstPtr& msg)
{
	ROS_INFO("+%s %d", __func__, msg->data);
	
	// TODO PF error, stop PF

	current_sm = CURRENT_IDLE;
}

static int get_control() {
	int i = -1;
	FILE *f;
	int ret = 0;
	f = fopen("/tmp/control", "r");

    if (!f) {
        printf("%s file open fail %d\n", __func__, errno);
		return -1;
    }

	fscanf(f, "%d", &i);
	printf("control=%d\n", i);
	fclose(f);
	return i;
}

// get voice txt- command list
static int read_config() {
    char line[255];
    char command[255];
    int code;
    FILE *f;
    int ret;
    int i, len;

    memset(&voice_commands, 0, sizeof(voice_commands));

	// file format: ֹͣ-2
    f = fopen("/etc/commands.txt", "re");

    if (!f) {
        printf("%s file open fail %d\n", __func__, errno);
		return -1;
    }

    //line = NULL;
    len = 0;
    while (fscanf(f, "%s[^\n]\n", (char *)&line) != EOF) {
        printf("line=[%s]\n", line);

        if (sscanf(line, "%[^-]-%d", (char *)&command, &code) != 2) {
        	printf("[%s] scan error! command=[%s] code=[%d]\n", line, command, code);
        	continue;
        }
        
        printf("command=[%s]%zu code=%d\n", command, strlen(command), code);
        strcpy(voice_commands[len].command, command);
        voice_commands[len].code = code;
        len++;
        
        //free(line);
        //line = NULL;
    }

    //for (i = 0; i < len; i++) 
    i = 0;
    while (strlen(voice_commands[i].command) != 0)
    {
        printf("{[%s]-%d}\n", voice_commands[i].command, voice_commands[i].code);
        i++;
    }

    fclose(f);    
  return 0;	
	
}

static int search_command(const char *command) {
	int i = 0;
	int code = -1;
	char *found = NULL;
	
	printf("+%s [%s]\n", __func__, command);

	i = 0;
	code = -1;
	while (0 != strlen(voice_commands[i].command)) {
		//printf("%d=[%s]\n", i, voice_commands[i].command);
		found = strstr((char *)command, voice_commands[i].command);
		if (found) {
			printf("%s %d find [%s]-[%s] code=%d\n", __func__, i, command, voice_commands[i].command, voice_commands[i].code);
			code = voice_commands[i].code;
			break;
		}
		i++;
	}

	printf("-%s [%s] get code=%d\n", __func__, command, code);
	return code;
}

static int search_command_index(const int code) {
	int i = 0;
	
	ROS_INFO("+%s [%d]\n", __func__, code);

	i = 0;
	while (0 != strlen(voice_commands[i].command)) {
		//printf("%d=[%s]\n", i, voice_commands[i].command);
		if (code == voice_commands[i].code) {
			ROS_INFO("-%s find code %d at %d\n", __func__, code, i);
			return i;
		}
		i++;
	}

	ROS_INFO("-%s cannot find code %d\n", __func__, code);
	return -1;
}

int main(int argc, char* argv[])
{
	char *found = NULL;
	// OR xyz
	std_msgs::Float32MultiArray OR_xyz;
	// robot move 
	geometry_msgs::Twist input_vel;

	// robot led
	kobuki_msgs::Led  robot_led;
	kobuki_msgs::Sound  robot_sound;

	char tts_content[255];
	int code = 0;
	int index = 0;
	int i = 0;
	int control = 0;
	static int locked_played = 0;
	static int unlocked_played = 0;
	// play back the received voice
	std_msgs::String msg_tts;
	//std::cout << "asr start ..." << endl; 
    ros::init(argc, argv, "xf_asr_node");

	ros::NodeHandle n;
	
	// OD service call 
	ros::ServiceClient od_client = n.serviceClient<demo_od::ObjectDetect>("object_detect_wrapper");
	demo_od::ObjectDetect od_srv;
	demo_od::ObjectDetect::Request od_req;
	demo_od::ObjectDetect::Response od_resp;

	// TTS service call 
	ros::ServiceClient client = n.serviceClient<voice_system::TTSService>("tts_service");
	voice_system::TTSService srv;

	ros::Subscriber sub_manual = n.subscribe("/voice/control", 50, manualCallback);
		
    //ros::Subscriber sub = n.subscribe("/voice/xf_asr_topic", 50, asrCallback);
    ros::Subscriber sub = n.subscribe("/face/auth", 50, faceCallback);

	// get results from other modules
	ros::Subscriber sub_results = n.subscribe("/voice/result_topic", 50, resultsCallback);
		
	// check if the tts/speaker is playing
    ros::Subscriber sub_ttsplay = n.subscribe("/voice/xf_tts_playing", 50, ttsplayCallback);
	
	// publish to tts, play back the received voice
	// switch to use service API, discard the pub
	//ros::Publisher pub_tts = n.advertise<std_msgs::String>("/voice/xf_tts_topic", 10);

	ros::Publisher pub_text = n.advertise<std_msgs::String>("/voice/tuling_nlu_topic", 50);

	// command for other modules
	ros::Publisher pub_cmd = n.advertise<std_msgs::Int32>("/voice/cmd_topic", 50);

	// control the robot move
	ros::Publisher pub_robot = n.advertise<geometry_msgs::Twist>("/mobile_base/commands/velocity", 1000); 

	// control the LED on robot kobuki_msgs/Led
 	ros::Publisher pub_robot_led = n.advertise<kobuki_msgs::Led>("/mobile_base/commands/led1", 10);
	
	// control the sound on robot
 	ros::Publisher pub_robot_sound = n.advertise<kobuki_msgs::Sound>("/mobile_base/commands/sound", 10);
	   
	// publish for ARM
	ros::Publisher pub_arm = n.advertise<std_msgs::Float32MultiArray>("/voice/manipulate_topic", 50);
	int manual_control = -1;

	ros::Rate loop_rate(10);

	ROS_INFO("start listen ... argc=%d", argc);

	if (argc == 2) {
		manual_control = strtol(argv[1], NULL, 10);
		if (manual_control > 0) {
			manual_control = 1; // enable manual control
			ROS_INFO("In MANUAL MODE...");
		}
	}

	read_config();
	//std::cout << "start listen ..." << endl;
	while (ros::ok())
	{
		if (0) {
			OR_xyz.data.clear();
			OR_xyz.data.push_back(1);
			OR_xyz.data.push_back(2);
			OR_xyz.data.push_back(3);
			pub_arm.publish(OR_xyz);
			goto DONE;
		}
		
		if (0) { // sound test
			ROS_INFO("i=%d", i);
			robot_sound.value = i;
			pub_robot_sound.publish(robot_sound);
			i++;
			if (i > 6) i = 0; 
			goto DONE;
		}		
		if (0) { // LED test
			ROS_INFO("i=%d", i);
			robot_led.value = i; //kobuki_msgs::Led::RED;
			pub_robot_led.publish(robot_led);
			i++;
			if (i > 3) i = 0; 
			goto DONE;
		}
	
		if (0) { // od service test
			// OD service call rosservice call /object_detect_wrapper "target: 'bottle'"
			od_req.target = "bottle";
			if (od_client.call(od_req, od_resp)) {
				ROS_INFO("call OD service okay");
			} else {
				ROS_INFO("call OD service fail");
			}
			ROS_INFO("x,y,z=%f-%f-%f result=%d", od_resp.x, 
				od_resp.y, od_resp.z, od_resp.result);
			//continue;
			goto DONE;
		}

		if (0) { // tts service test
			#if 1
				TTS_TEXT("认证失败！系统被锁定");
			#else
				memset(tts_content, 0, sizeof(tts_content));
				strcat(tts_content, "认证失败！系统被锁定");
				srv.request.target = tts_content;
				if (client.call(srv)) {
					ROS_INFO("call TTS service okay");
				} else {
					ROS_INFO("call TTS service fail");
				}
			#endif
			goto DONE;
		}
	
		if (g_result) {
			free(g_result);
			g_result = NULL;
		}
	
		// listen .. 
		if (sys_locked == 1) {
			//ROS_INFO("sys_locked, skip voice!");
			if (locked_played == 0) {
				locked_played = 1;
				if (1 == manual_control) {
					system("play /tmp/locked.wav");
				} else {
					TTS_TEXT("认证失败！系统被锁定");			
				}
			}
			//continue;
		}
		else if (sys_locked == 0) { // FR PASS
			if (unlocked_played == 0) {
				unlocked_played = 1;
				if (1 == manual_control) {
					system("play /tmp/unlocked.wav");
				} else {
					TTS_TEXT("认证通过！欢迎使用ROS机器人");
				}
			} else {
				
			}

			if (manual_control == 1) {
				// set g_result by manual_code;
				if (voice_manual_code > -1) {
					ROS_INFO("MANUALcode=%d", voice_manual_code);
					asr_flag = 1;
					index = search_command_index(voice_manual_code);
					ROS_INFO("control=%d index=%d", voice_manual_code, index);
				    g_result = (char*)realloc(g_result, strlen(voice_commands[index].command) + 10);					
					sprintf(g_result, "机器人%s", voice_commands[index].command);
					printf("NEW voice=[%s] len=%zu\n", g_result, strlen(g_result));
					voice_manual_code = -1;
				} else { // no vice input
					asr_flag = 0;
					if (g_result) {
						free(g_result);
						g_result = NULL;
					}
				}
			} else {
				// use the LED/sound for mic start
				robot_sound.value = 1;
				pub_robot_sound.publish(robot_sound);
				
				robot_led.value = kobuki_msgs::Led::RED;
				pub_robot_led.publish(robot_led);
				asrProcess();
				robot_led.value = kobuki_msgs::Led::BLACK;
				pub_robot_led.publish(robot_led);
			}
		} else {
			ROS_INFO("sys_locked=%d", sys_locked);
		}
		
		if (0)
		{	// rostopic pub -v -1 /mobile_base/commands/velocity geometry_msgs/Twist -- '[0.1,0,0]' '[0,0,0]'
			ROS_INFO("move...");
			input_vel.linear.x=0.0; //forward/back
			input_vel.linear.y=0.0;
			input_vel.linear.z=0.0;
			input_vel.angular.x=0.0;
			input_vel.angular.y=0.0;
			input_vel.angular.z=0.3; //left/right
			pub_robot.publish(input_vel);
			//continue;
		}
		// get voice result
		//ROS_INFO("asr_flag=%d current_sm=%d sys_locked=%d g_result=%p", asr_flag, current_sm, sys_locked, g_result);
		if (asr_flag) {
			std_msgs::String msg;
			std_msgs::Int32 cmd_msg;

			if ((g_result == NULL) || (strlen(g_result) < 2)) {
				ROS_INFO("no voice detected g_result=%p", g_result);
				goto DONE;
			}
			
			printf("voice=[%s] len=%zu current_sm=%d\n", g_result, strlen(g_result), current_sm);
						
			if (strlen(g_result) > 100) {
				ROS_INFO("too many commands");
				goto DONE;
			}
			
			if (strlen(g_result) < 7) {
				ROS_INFO("too short commands");
				goto DONE;
			}
#if 0			
			// add manual control
			control = get_control();
			if (control > -1) {
				index = search_command_index(control);
				ROS_INFO("control=%d index=%d", control, index);
			    g_result = (char*)realloc(g_result, strlen(voice_commands[index].command) + 10);					
				sprintf(g_result, "机器人%s", voice_commands[index].command);
				printf("NEW voice=[%s] len=%zu\n", g_result, strlen(g_result));
			}
#endif
			found = strstr(g_result, "机器人");
			if (found) {
				// messages for robot get content
				delStr(g_result, "机器人");
				printf("voice_command=[%s]\n", g_result);
			} else {
				ROS_INFO("skip ...");
				goto DONE;
			}

			msg.data = g_result;

			code = search_command(g_result);

			// code=0, stop all actions!
			if (code == 0) { // stop movement
				current_sm = CURRENT_IDLE;
				input_vel.linear.x=0.0; //forward/back
				input_vel.linear.y=0.0;
				input_vel.linear.z=0.0;
				input_vel.angular.x=0.0;
				input_vel.angular.y=0.0;
				input_vel.angular.z=0.0; //left/right
				pub_robot.publish(input_vel);
			}
			
			if (code >= 0) { // the voice is a special command
				cmd_msg.data = code;
				index = search_command_index(code);
				ROS_INFO("command [%d] index=%d", code, index);
	
				if (code == 61) {
					// no need to speak out
				} else {
					if (1 == manual_control) {
					    memset(tts_content, 0, sizeof(tts_content));
						sprintf(tts_content, "play /tmp/%d.wav", code);
						ROS_INFO("exec [%s]", tts_content);
						system(tts_content);
					} else {
						memset(tts_content, 0, sizeof(tts_content));
						sprintf(tts_content, "执行命令 %s", voice_commands[index].command);
						srv.request.target = tts_content;
						if (client.call(srv)) {
							ROS_INFO("call TTS service okay");
						} else {
							ROS_INFO("call TTS service fail");
						}						
					}
				}

				if (code <= 100) { // pf/vslam/or
					switch (code) {
						case 0:// stop command, send out in any conditions 
							current_sm = CURRENT_IDLE;
							pub_cmd.publish(cmd_msg);
							break;
						case 1: // PF command check if it is oaky to send out this command
							if (current_sm == CURRENT_IDLE) {
								current_sm = CURRENT_PF;
								pub_cmd.publish(cmd_msg);			
							}
							break;
						case 2: // stop PF
						case 21:
							if (current_sm == CURRENT_PF) {
								current_sm = CURRENT_IDLE;
								cmd_msg.data = 2;
								pub_cmd.publish(cmd_msg);
							}
							break;
						case 3: // start VSLAM
							if (current_sm == CURRENT_IDLE) {
								current_sm = CURRENT_VSLAM;
								pub_cmd.publish(cmd_msg);
							}
							break;
						case 4: // stop VSLAM
							if (current_sm == CURRENT_VSLAM) {
								current_sm = CURRENT_IDLE;
								pub_cmd.publish(cmd_msg);
							}
							break;
						case 5: // bye-bye system lock and start FR, and stop all other running tasks
						case 51:
							if (current_sm == CURRENT_VSLAM) {
								// stop VSLAM
								cmd_msg.data = 4;
								pub_cmd.publish(cmd_msg);
							}
							if (current_sm == CURRENT_PF) {
								// stop PF
								cmd_msg.data = 2;
								pub_cmd.publish(cmd_msg);	
							}
							current_sm = CURRENT_IDLE;
							sys_locked = -1;
							asr_flag = 0;
							locked_played = 0;
							unlocked_played = 0;
							if (g_result) {
								free(g_result);
								g_result = NULL;
							}
							sleep(1); // make sure tasks are stop
							cmd_msg.data = 5;
							pub_cmd.publish(cmd_msg); // start FR task
							break;
						case 6: // arm move
						case 61:
							//if (current_sm == CURRENT_IDLE) {
								//current_sm = CURRENT_IDLE;
								cmd_msg.data = 6;
								pub_cmd.publish(cmd_msg);
							//}
							break;
						case 7: // open HAND
							if (current_sm != CURRENT_HAND_OPEN) {
								current_sm = CURRENT_HAND_OPEN;
								pub_cmd.publish(cmd_msg);
							}
							break;
						case 8: // 	close hand
							if (current_sm == CURRENT_HAND_OPEN) {
								current_sm = CURRENT_HAND_CLOSE;
								pub_cmd.publish(cmd_msg);
							}
							break;
						case 9:// OR
						case 91:
						case 92:
						case 93:
						case 94:
						case 95:
						{
							char object_finding[255];
							char *found_object = NULL;
							int i = 0;
							i = 0;
							// start to find object TTS
							while (strlen(objects[i].name1)) {
								ROS_INFO("%s-%s\n", objects[i].name1, objects[i].name2);
								printf("%s-%s\n", objects[i].name1, objects[i].name2);
							    found_object = strstr(g_result, objects[i].name1);
								if (found_object) {
									ROS_INFO("start find %s", objects[i].name2);
									od_req.target = objects[i].name2; 
									if (od_client.call(od_req, od_resp)) {
										ROS_INFO("call OD service okay");
									} else {
										ROS_INFO("call OD service fail");
									}
									ROS_INFO("x,y,z=%f-%f-%f result=%d", od_resp.x, 
										od_resp.y, od_resp.z, od_resp.result);
									//break;
									memset(object_finding, 0, sizeof(object_finding));
									
									if (od_resp.result) {
										if (1 == manual_control) {
											system("play /tmp/found.wav");
										} else {
											TTS_TEXT("已找到");
										}
										OR_xyz.data.clear();
										OR_xyz.data.push_back(od_resp.x);
										OR_xyz.data.push_back(od_resp.y);
										OR_xyz.data.push_back(od_resp.z);
									pub_arm.publish(OR_xyz);
									} else {
										if (1 == manual_control) {
											system("play /tmp/notfound.wav");
										} else {
											TTS_TEXT("未找到");
										}
									}
								}
								i++;
							}
							break;
						}
						default:
							break;
					}
					
				} else { // code > 100, control commands
					if ((current_sm == CURRENT_PF)
						|| (current_sm == CURRENT_VSLAM)) {
							ROS_INFO("CANNOT move %d", current_sm);
						} 
						else {
						ROS_INFO("move %d...", code);
						input_vel.linear.x=0.0; //forward/back
						input_vel.linear.y=0.0;
						input_vel.linear.z=0.0;
						input_vel.angular.x=0.0;
						input_vel.angular.y=0.0;
						input_vel.angular.z=0.0; //left/right
						switch (code) {
							case 300:
								input_vel.angular.z = 1.9;
								break;
							case 400:
								input_vel.angular.z = -1.9;
								break;
							case 500:
							case 501:
								input_vel.linear.x = 0.3;
								break;
							case 600:
							case 601:
								input_vel.linear.x = -0.3;
								break;
							default:
								break;
						}
						pub_robot.publish(input_vel);
						current_sm = CURRENT_IDLE;
					}
				}
				//sleep(8);
			} else { // unknown code, send to tuling
				bool can_send = false;
				printf("unknow code [%s]\n", g_result);
				//delStr(g_result, "机器人");
				//printf("publish to tuling [%s]\n", g_result);
				//deleteChars(g_result, "机器人");
				// FIXME filter
				found = strstr(g_result, "今天"); 
				if (found) {
					can_send = true;
				}
				
				found = strstr(g_result, "日期");
				if (found) {
					can_send = true;
				}
				
				found = strstr(g_result, "时间");
				if (found) {
					can_send = true;
				}
				
				found = strstr(g_result, "天气");
				if (found) {
					can_send = true;
				}
				
				found = strstr(g_result, "名字");
				if (found) {
					can_send = true;
				}
				
				found = strstr(g_result, "笑话");
				if (found) {
					can_send = true;
				}
				
				found = strstr(g_result, "故事");
				if (found) {
					can_send = true;
				}
				
				if (1 == manual_control) {
					
				} else {
					if (can_send) {
						TTS_TEXT("请稍等");
						msg.data = g_result;
						pub_text.publish(msg); // send to tuling
						goto DONE;
					}
					
					{
						TTS_TEXT("请再说一遍");
					}
				}
				//sleep(8);
			} // unknown code, send to tuling
			//speech_end = true;
			//asr_flag =0;
		}else {
			//ROS_INFO("asr_flag=false");
		}
DONE:		
		loop_rate.sleep();
        ros::spinOnce();
	 }

	return 0;
}
