clear all
close all
clc

img = imread('test.jpg');
Xform = [0.000100990123328 0 0;0.000412396945637 0.001302203393162 1.293171994e-06;-0.103019798961327 -0.255811259450009 -0.000222053779501];

% Xform = Xform*tr;

R = imref2d([480 640],1, 1);    %The warped image extends beyond the original size. Specify the output image size and its location relevant to the origin (determined by the Xform)
B = imwarp(img,projective2d(Xform),'OutputView',R);
% B = imwarp(img,projective2d(Xform));
imwrite(B,'SW_Full_Precision.jpg');
imshow(B)
title('Full Precision');

[A,Err_r,Err_c] = projective_warp(img, Xform,[480 640],[0 0],0,1);
Err = [Err_r,Err_c];

imwrite(A,'HW_Approx_Precision.jpg');
figure
imshow(A)
title('Low Precision (Approx.)');

% theta = 15*pi/180;
% h = [cos(theta) -sin(theta) 0; sin(theta) cos(theta) 0; 0 0 1];
% h = [1 0 0; 0 1 0; 0 0 1];


% figure
% imshow(img)
% [x,y] = ginput(21);
% pts_in = [x,y];
% imshow(B)
% clear x
% clear y
% [x,y] = ginput(21);
% pts_out = [x,y];
%  
% tForm = fitgeotrans(pts_in,pts_out,'projective')
% tForm.T
% h = find_homography(pts_in, pts_out)
% h_inv = find_homography(pts_out, pts_in);





