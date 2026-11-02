# TTGO T10 & T4 TEST CODE

# [EN](./README.Md) | 中文

# 快速开始

1. 从Arduino 库安装管理器中安装`TFT_eSPI`
2. 将`LilyGo_Txx/lib`目录的`Buttons`拷贝到`<C:\Users\UserName\Documents\Arduino\libraries>`目录内
3. 进入<C:\Users\UserName\Documents\Arduino\libraries\TFT_eSPI>更改配置文件,对于不同型号的板子需要开启对应的`TFT_eSPI`设置(选择对应的LCD屏幕的引脚).

#### **T4 V1.3**

- 在`TFT_eSPI`目录里打开`User_Setup_Select.h`,将`#include <User_Setup.h>`将它禁用.
- 将`#include <User_Setups/Setup22_TTGO_T4_v1.3.h>` 行取消注释

#### **T4 V1.2**

- 在`TFT_eSPI`目录里打开`User_Setup_Select.h`,将`#include <User_Setup.h>`将它禁用.
- 将`#include <User_Setups/Setup22_TTGO_T4.h>` 行取消注释

#### **TS V2.0 1.4 Inc'**

- 将`LilyGo_Txx`文件夹内`User_Setups/T10_V20_1C4.h`拷贝到`TFT_eSPI\User_Setups`目录内
- 在`TFT_eSPI`目录里打开`User_Setup_Select.h`,将`#include <User_Setup.h>`将它禁用,在`#include <User_Setup.h> `行下方添加`#include <User_Setups/T10_V20_1C4.h>`

#### **TS V2.0 1.8 Inc'**

- 将`LilyGo_Txx`文件夹内`User_Setups/T10_V20_1C8.h`拷贝到`TFT_eSPI\User_Setups`目录内
- 在`TFT_eSPI`目录里打开`User_Setup_Select.h`,将`#include <User_Setup.h>`将它禁用,在`#include <User_Setup.h> `行下方添加`#include <User_Setups/T10_V20_1C8.h>`

3. 打开`LilyGo_Txx/LilyGo_Txx.ino`，在草图上方,取消对应板子的注释,看起来如下
    
    ```
    // #define T4_V12       //Historical version
    // #define T4_V13       //Version for sale
    // #define T10_V14      //Historical version
    // #define T10_V18      //Version for sale  TS V1.8
    // #define T10_V20      //Version for sale  TS V2.0
    ```

4. 按照下列选项进行板子的选择
    1. `Board` -> `ESP32` -> `ESP32 Dev Module`
    2. `PSRAM` -> `Enable` (如果是TS,这里需要选择Disable)
    3. `Flash Size` -> `4MB(32Mb)`
    4. `Flash Mode` -> `DIO`
    5. 其他一些选择根据实际填写,选择完如下图所示
        ![](./images/setting.jpg)


5. 点击`Arduino IDE` 上方向右箭头,进行编译和上传(请确保安装了串口驱动和将板子通过USB连接到了电脑)




## T10 pin map

| Name        | V18/V20 |
| ----------- | ------- |
| TFT Driver  | ST7735  |
| TFT_MISO    | N/A     |
| TFT_MOSI    | 23      |
| TFT_SCLK    | 5       |
| TFT_CS      | 16      |
| TFT_DC      | 17      |
| TFT_RST     | N/A     |
| TFT_BL      | 27      |
| SD_MISO     | 2       |
| SD_MOSI     | 15      |
| SD_SCLK     | 14      |
| SD_CS       | 13      |
| I2C_SDA     | 21      |
| I2C_SCL     | 22      |
| SPEAKER_OUT | 25      |
| ADC_IN      | 35      |
| BUTTON1     | 36      |
| BUTTON2     | 37      |
| BUTTON3     | 39      |

## T4  pin map

| Name        | V12     | V13     |
| ----------- | ------- | ------- |
| TFT Driver  | ILI9341 | ILI9341 |
| TFT_MISO    | 12      | 12      |
| TFT_MOSI    | 23      | 23      |
| TFT_SCLK    | 18      | 18      |
| TFT_CS      | 27      | 27      |
| TFT_DC      | 26      | 32      |
| TFT_RST     | 5       | 5       |
| TFT_BL      | N/A     | 4       |
| SD_MISO     | 2       | 2       |
| SD_MOSI     | 15      | 15      |
| SD_SCLK     | 14      | 14      |
| SD_CS       | 13      | 13      |
| Button 0    | 14      | 14      |
| Button 1    | 5       | 5       |
| Button 2    | 27      | 27      |
| Button 3    | 25      | 25      |
| SPEAKER PWD | N/A     | 19      |
| SPEAKER OUT | N/A     | 25      |
| I2C_SDA     | 21      | 21      |
| I2C_SCL     | 22      | 22      |
