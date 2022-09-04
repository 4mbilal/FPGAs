clear all
close all
clc

load('lut_rc.mat');
lut_rc_c = lut_rc(:,:,1);
lut_rc_r = lut_rc(:,:,2);

s = size(lut_rc_c);
r = zeros(1,s(1)*s(2))';
c = zeros(1,s(1)*s(2))';
z = zeros(1,s(1)*s(2))';
y = zeros(1,s(1)*s(2))';
k = 1;
for i=1:s(1),
    for j=1:s(2),
        r(k) = i;
        c(k) = j;
        z(k) = lut_rc_c(i,j);
        y(k) = lut_rc_r(i,j);
        k = k + 1;
    end
end

% sf = fit([r, c],z,'linearinterp')%poly11')
sf_c = fit( [r, c], z, 'poly33', 'Robust', 'Bisquare' )
sf_r = fit( [r, c], y, 'poly22', 'Robust', 'LAR' )

% plot(sf,[r,c],z)
% 
h_inv = [0.3167   -0.3167  217.9075; -0.0000   -0.1705  196.4449; -0.0000   -0.0010    1.0000];

% syms c r
% fr = (h_inv(2,1)*c + h_inv(2,2)*r + h_inv(2,3))/(1 + h_inv(3,2)*r);
% taylor(fr, [r c], [0 0], 'Order',4)
% 
% fc = (h_inv(1,1)*c + h_inv(1,2)*r + h_inv(1,3))/(1 + h_inv(3,2)*r);
% taylor(fc, [r c], [208 320], 'Order',4)
