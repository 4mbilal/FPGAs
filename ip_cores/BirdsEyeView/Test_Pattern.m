clear all
close all
clc

r = 480;
c = 640;
img = zeros(r,c);
block_size = [80,80];

test_block = zeros(block_size(1),block_size(2),2);
test_block_white = zeros(block_size);
test_block_black = zeros(block_size);
 
for i=1:block_size(1),
    for j=1:block_size(2),
        x = i - block_size(1)/2;
        y = j - block_size(2)/2;
        test_block(i,j,1) = 255*cos(0.5*(x*y)/block_size(1));
        test_block(i,j,2) = 255-255*cos(0.5*(x*y)/block_size(1));
    end
end


ti = 0;
for i=1:block_size(1):r,
    tj = ti;
    for j=1:block_size(2):c,
        %Simplge Checkerboard Pattern (Black & White)
%         img(i:i+block_size(1)-1,j:j+block_size(2)-1) = tj*255;
        %A Cosine function inside a Checkerboard for grayscale values
        img(i:i+block_size(1)-1,j:j+block_size(2)-1) = test_block(:,:,tj+1);
%         img(i:end,j:end) = tj*255;
        tj = 1-tj;
    end
    ti = 1-ti;
end

img = uint8(img);
imwrite(img,'Checkerboard.bmp','BMP');
imshow(img)