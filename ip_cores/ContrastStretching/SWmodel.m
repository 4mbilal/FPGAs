clear all
close all
clc

img = rgb2gray(imread('D:\datasets\test_images\einstein_dark_640x480.jpg'));
imshow(img)
pixels_in = img(:);
pixels_out = pixels_in;
totCount = length(pixels_in);

prob = 0.05;
upperLimit = 250;
lowerLimit = 5;
upperCount = 0;
lowerCount = 0;

while(1)
    if(lowerCount>prob*totCount)
       lowerLimit = lowerLimit - 1;
    else
       lowerLimit = lowerLimit + 1;
    end
    if(upperCount>prob*totCount)
       upperLimit = upperLimit + 1;
    else
       upperLimit = upperLimit - 1;
    end
    upperCount = 0;
    lowerCount = 0;
    for i=1:totCount
        p = double(pixels_in(i));
        out = ((p-lowerLimit)/(upperLimit-lowerLimit))*255;
        pixels_out(i) = max(min(out,255),0);
        if(p<lowerLimit)
            lowerCount = lowerCount + 1;
        end
        if(p>upperLimit)
            upperCount = upperCount + 1;
        end
    end
    img_out = uint8(reshape(pixels_out,480,640));
    subplot(2,2,1)
    imshow(img)
    subplot(2,2,2)
    imhist(img)
    subplot(2,2,3)    
    imshow(img_out)
    subplot(2,2,4)
    imhist(img_out)
    pause(0.1)
end