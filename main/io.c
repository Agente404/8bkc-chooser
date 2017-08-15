#include <string.h>
#include <stdio.h>
#include <sdkconfig.h>
#include "rom/ets_sys.h"
#include "soc/gpio_reg.h"
#include "soc/dport_reg.h"
#include "soc/rtc_cntl_reg.h"
#include "driver/rtc_io.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io.h"
#include "ssd1331.h"
#include "hw.h" //from gnuboy, for PAD_* defines
#include "esp_deep_sleep.h"
#include "driver/rtc_io.h"

//Buttons. Pressing a button pulls down the associated GPIO
#define GPIO_BTN_RIGHT (1<<21)
#define GPIO_BTN_LEFT ((uint64_t)1<<39)
#define GPIO_BTN_UP ((uint64_t)1<<34)
#define GPIO_BTN_DOWN ((uint64_t)1<<35)
#define GPIO_BTN_B (1<<4)
#define GPIO_BTN_A (1<<16)
#define GPIO_BTN_SELECT (1<<25)
#define GPIO_BTN_START (1<<27)
#define GPIO_BTN_PWR_PIN 32
#define GPIO_BTN_PWR ((uint64_t)1<<GPIO_BTN_PWR_PIN)

//OLED connections
#define GPIO_OLED_CS_PIN 5
#define GPIO_OLED_CLK_PIN 18
#define GPIO_OLED_DAT_PIN 23

#define GPIO_OLED_RST ((uint64_t)1UL<<33UL)
#define GPIO_OLED_CS (1<<GPIO_OLED_CS_PIN)
#define GPIO_OLED_CLK (1<<GPIO_OLED_CLK_PIN)
#define GPIO_OLED_DC (1<<22)
#define GPIO_OLED_DAT (1<<GPIO_OLED_DAT_PIN)

#define CONFIG_GBFEMTO_HW_VER 1

#if (CONFIG_GBFEMTO_HW_VER == 0)
//PWMA/PWMB for sound
#define GPIO_PWMB (1<<19)
#define GPIO_PWMA (1<<26)
#define GPIO_DAC 0
#define GPIO_CHGDET (1<<17) //battery is charging
#define GPIO_STDBY 0        //micro-usb plugged and bat full
#define GPIO_VBAT 0
#define GPIO_14VEN_PIN 2 //High enables 14V generation for OLED
#define GPIO_OLED_PWR 0
#else
//DAC for sound
#define GPIO_PWMB 0
#define GPIO_PWMA 0
#define GPIO_DAC (1<<26)
//#define GPIO_CHGDET (1<<19) //battery is charging
//#define GPIO_STDBY (1<<14)  //micro-usb plugged and bat full
#define GPIO_CHGDET (1<<19) //battery is charging, low-active
#define GPIO_CHGSTDBY ((uint64_t)1<<36)  //micro-usb plugged
#define GPIO_VBAT ((uint64_t)1<<36)
#define GPIO_14VEN_PIN 17
#define GPIO_OLED_PWR ((1<<2)|(1<<13))
#endif

#define GPIO_14VEN (1<<GPIO_14VEN_PIN) //High enables 14V generation for OLED (and audio amp)




#define OLED_SPI_NUM HSPI_HOST

spi_device_handle_t oled_spi_handle;

void oled_spi_pre_transfer_callback(spi_transaction_t *t) 
{
	int dc=(int)t->user;
	WRITE_PERI_REG(dc?GPIO_OUT_W1TS_REG:GPIO_OUT_W1TC_REG, GPIO_OLED_DC);
}


void ioOledSend(char *data, int count, int dc) {
	esp_err_t ret;
	spi_transaction_t t;
	if (count==0) return;             //no need to send anything
	memset(&t, 0, sizeof(t));       //Zero out the transaction
	t.length=count*8;                 //Len is in bytes, transaction length is in bits.
	t.tx_buffer=data;               //Data
	t.user=(void*)dc;               //D/C info for callback
	ret=spi_device_transmit(oled_spi_handle, &t);  //Transmit!
	assert(ret==ESP_OK);            //Should have had no issues.
}

int ioGetChgStatus() {
	uint64_t io=((uint64_t)GPIO.in1.data<<32)|GPIO.in;
	if ((io&GPIO_CHGSTDBY)==0) {
		return IO_CHG_NOCHARGER;
	} else {
		if ((io&GPIO_CHGDET)==0) {
			return IO_CHG_CHARGING;
		} else {
			return IO_CHG_FULL;
		}
	}
}


int ioJoyReadInput() {
	int i=0;
	static int initial=1;
	static int powerWasPressed=0;
	static uint32_t powerPressedTime;
	uint64_t io=((uint64_t)GPIO.in1.data<<32)|GPIO.in;
	//Ignore remnants from 1st power press
	if ((io&GPIO_BTN_PWR)) {
		if (!initial) {
			i|=0x100;
			if (!powerWasPressed) powerPressedTime=xTaskGetTickCount();
			//Quit emu after being pressed for 2 seconds
			if ((xTaskGetTickCount()-powerPressedTime)>(2000/portTICK_PERIOD_MS)) {
			}
			//Force power down after being pressed for 6 seconds.
			if ((xTaskGetTickCount()-powerPressedTime)>(6000/portTICK_PERIOD_MS)) {
				ioPowerDown();
			}
			powerWasPressed=1;
		}
	} else {
		initial=0;
		powerWasPressed=0;
	}
	if (!(io&GPIO_BTN_RIGHT)) i|=PAD_RIGHT;
	if (!(io&GPIO_BTN_LEFT)) i|=PAD_LEFT;
	if (!(io&GPIO_BTN_UP)) i|=PAD_UP;
	if (!(io&GPIO_BTN_DOWN)) i|=PAD_DOWN;
	if (!(io&GPIO_BTN_SELECT)) i|=PAD_SELECT;
	if (!(io&GPIO_BTN_START)) i|=PAD_START;
	if (!(io&GPIO_BTN_A)) i|=PAD_A;
	if (!(io&GPIO_BTN_B)) i|=PAD_B;
//	printf("%x\n", i);
	return i;
}

//Usually called from joybtn handler, so the emu should be 'stopped' already.
void ioPowerDown() {
	vTaskDelay(20/portTICK_PERIOD_MS); //Allow video thread to finish write
	printf("PowerDown: wait till power btn is released...\n");
	while(1) {
		uint64_t io=((uint64_t)GPIO.in1.data<<32)|GPIO.in;
		vTaskDelay(50/portTICK_PERIOD_MS);
		if (!(io&GPIO_BTN_PWR)) break;
	}
	printf("PowerDown: Powering down.\n");
	vTaskDelay(1000/portTICK_PERIOD_MS);
#if (CONFIG_GBFEMTO_HW_VER == 0)
	WRITE_PERI_REG(GPIO_OUT_W1TC_REG, GPIO_14VEN);
#else
	WRITE_PERI_REG(GPIO_OUT_W1TS_REG, GPIO_14VEN);
#endif

	esp_deep_sleep_enable_ext1_wakeup(GPIO_BTN_PWR|GPIO_CHGSTDBY, ESP_EXT1_WAKEUP_ANY_HIGH);
//	esp_deep_sleep_enable_ext0_wakeup(GPIO_BTN_PWR_PIN, 1);
//	esp_deep_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
	esp_deep_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
/*
	rtc_gpio_pulldown_en(GPIO_14VEN_PIN); //14V gen
	rtc_gpio_hold_en(GPIO_14VEN_PIN);
	rtc_gpio_pullup_en(GPIO_OLED_CS_PIN); //cs
	
	rtc_gpio_pullup_en(GPIO_OLED_CLK_PIN); //clk
	rtc_gpio_pullup_en(22); //dc
	rtc_gpio_pullup_en(GPIO_OLED_DAT_PIN); //dat
	rtc_gpio_pullup_en(33); //reset
*/

//	REG_SET_BIT(GPIO_PIN_MUX_REG[GPIO_14VEN_PIN], FUN_PD);
//	REG_SET_BIT(GPIO_PIN_MUX_REG[GPIO_OLED_CS_PIN], FUN_PU);
//	REG_SET_BIT(GPIO_PIN_MUX_REG[GPIO_OLED_CLK_PIN], FUN_PU);
//	REG_SET_BIT(GPIO_PIN_MUX_REG[GPIO_OLED_DAT_PIN], FUN_PU);

	//Force digital pins to hold their state
//	CLEAR_PERI_REG_MASK(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_DG_PAD_FORCE_UNHOLD);
//	  SET_PERI_REG_MASK(RTC_CNTL_DIG_ISO_REG, RTC_CNTL_DG_PAD_FORCE_HOLD);


	printf("PowerDown: esp_deep_sleep_start.\n");
	esp_deep_sleep_start();
	printf("PowerDown: after deep_sleep_start, huh?\n");
	while(1);
}

void ioInit() {
	int x;
	esp_err_t ret;
	spi_bus_config_t buscfg={
		.miso_io_num=-1,
		.mosi_io_num=GPIO_OLED_DAT_PIN,
		.sclk_io_num=GPIO_OLED_CLK_PIN,
		.quadwp_io_num=-1,
		.quadhd_io_num=-1,
        .max_transfer_sz=4096*3
	};
	spi_device_interface_config_t devcfg={
		.clock_speed_hz=10000000,               //Clock out at 10 MHz
		.mode=0,                                //SPI mode 0
		.spics_io_num=GPIO_OLED_CS_PIN,         //CS pin
		.queue_size=7,                          //We want to be able to queue 7 transactions at a time
		.pre_cb=oled_spi_pre_transfer_callback,  //Specify pre-transfer callback to handle D/C line
	};

	gpio_config_t io_conf[]={
		{
			.intr_type=GPIO_INTR_DISABLE,
			.mode=GPIO_MODE_OUTPUT,
			.pin_bit_mask=GPIO_OLED_RST|GPIO_OLED_CS|GPIO_OLED_CLK|GPIO_OLED_DC|GPIO_OLED_DAT|GPIO_PWMA|GPIO_PWMB|GPIO_OLED_PWR
		},
		{
			.intr_type=GPIO_INTR_DISABLE,
			.mode=GPIO_MODE_INPUT,
			.pull_up_en=1,
			.pin_bit_mask=GPIO_BTN_RIGHT|GPIO_BTN_LEFT|GPIO_BTN_UP|GPIO_BTN_DOWN|GPIO_BTN_B|GPIO_BTN_A|GPIO_BTN_SELECT|GPIO_BTN_START|GPIO_CHGDET
		},
		{
			.intr_type=GPIO_INTR_DISABLE,
			.mode=GPIO_MODE_OUTPUT_OD,
			.pull_up_en=1,
			.pin_bit_mask=GPIO_14VEN
		},
		{
			.intr_type=GPIO_INTR_DISABLE,
			.mode=GPIO_MODE_INPUT,
//#if (CONFIG_GBFEMTO_HW_VER == 0)
			.pull_down_en=1,
//#else
//			.pull_up_en=1,
//#endif
			.pin_bit_mask=GPIO_BTN_PWR|GPIO_CHGSTDBY
		}
	};
	//Connect all pins to GPIO matrix
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO21_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO39_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO34_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO35_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO4_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO16_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO25_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO27_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO36_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO33_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO22_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO23_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO18_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO5_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO19_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO26_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTCK_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO17_U,2);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U,2);

	//In the current esp-idf, gpio 32/33 are hooked up as 32KHz xtal pins. Reset that.
	//Note: RTC_IO_X32N_MUX_SEL|RTC_IO_X32P_MUX_SEL need to be 0 instead of 1, as the trm notes.
	WRITE_PERI_REG(RTC_IO_XTAL_32K_PAD_REG, 0);

	for (x=0; x<sizeof(io_conf)/sizeof(io_conf[0]); x++) {
		gpio_config(&io_conf[x]);
	}

	WRITE_PERI_REG(GPIO_OUT_W1TC_REG, GPIO_OLED_PWR|GPIO_OLED_RST|GPIO_OLED_CS|GPIO_OLED_CLK|GPIO_OLED_DC|GPIO_OLED_DAT|GPIO_14VEN|GPIO_PWMA|GPIO_PWMB);

#if 0
	//from before dma spi code?
	gpio_matrix_out(23, VSPID_OUT_IDX,0,0);
	#ifndef VSPICLK_OUT_MUX_IDX
	#define VSPICLK_OUT_MUX_IDX VSPICLK_OUT_IDX
	#endif
	gpio_matrix_out(18, VSPICLK_OUT_MUX_IDX,0,0);
	gpio_matrix_out(5, VSPICS0_OUT_IDX,0,0);
#endif

#if 0
	//PWM outputs are driven by RMT
	gpio_matrix_out(19, RMT_SIG_OUT0_IDX, 0, 0);
	gpio_matrix_out(26, RMT_SIG_OUT4_IDX, 0, 0);
#endif

	//Kill power to oled to make sure it's reset
	WRITE_PERI_REG(GPIO_OUT_W1TC_REG, GPIO_OLED_PWR);
	WRITE_PERI_REG(GPIO_OUT1_W1TC_REG, GPIO_OLED_RST>>32);
	WRITE_PERI_REG(GPIO_OUT_W1TC_REG, GPIO_OLED_CS);
	WRITE_PERI_REG(GPIO_OUT_W1TC_REG, GPIO_OLED_CLK);
	WRITE_PERI_REG(GPIO_OUT_W1TC_REG, GPIO_OLED_DC);
	WRITE_PERI_REG(GPIO_OUT_W1TC_REG, GPIO_OLED_DAT);
	WRITE_PERI_REG(GPIO_OUT_W1TS_REG, GPIO_14VEN);
	vTaskDelay(300 / portTICK_PERIOD_MS);
	
	//Enable 3.3V power to OLED
	WRITE_PERI_REG(GPIO_OUT_W1TS_REG, GPIO_OLED_PWR);
	WRITE_PERI_REG(GPIO_OUT1_W1TS_REG, (GPIO_OLED_RST)>>32);
	//Wait till power stabilizes
	vTaskDelay(30 / portTICK_PERIOD_MS);

	//Reset OLED
	WRITE_PERI_REG(GPIO_OUT1_W1TC_REG, (GPIO_OLED_RST)>>32);
	vTaskDelay(10 / portTICK_PERIOD_MS);
	WRITE_PERI_REG(GPIO_OUT1_W1TS_REG, (GPIO_OLED_RST)>>32);
	vTaskDelay(10 / portTICK_PERIOD_MS);


	//Enable 14V and initialize controller
#if (CONFIG_GBFEMTO_HW_VER == 0)
	WRITE_PERI_REG(GPIO_OUT_W1TS_REG, GPIO_14VEN);
#else
	WRITE_PERI_REG(GPIO_OUT_W1TC_REG, GPIO_14VEN);
#endif
	vTaskDelay(300 / portTICK_PERIOD_MS);

	//Initialize the SPI bus
	ret=spi_bus_initialize(OLED_SPI_NUM, &buscfg, 1);
	assert(ret==ESP_OK);
	//Attach the LCD to the SPI bus
	ret=spi_bus_add_device(OLED_SPI_NUM, &devcfg, &oled_spi_handle);
	assert(ret==ESP_OK);

	ssd1331Init();

}

